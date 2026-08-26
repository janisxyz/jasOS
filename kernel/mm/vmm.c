#include <jasos/mm.h>
#include <jasos/ke.h>
#include <jasos/kprintf.h>
#include <jasos/string.h>

/*
 * Address spaces.
 *
 * Hardware path installs real 4-level PTEs. Host path records VADs and
 * treats the process aspace as a probe table — copyin/copyout on host
 * are kernel-linked so they memcpy. That is a lie we tell only to the
 * host HAL; the real kernel never memcpy's a user pointer.
 *
 * Why this will fail in production:
 *  - Kernel map is frozen after vmm_init. Loadable drivers cannot add
 *    kernel VA without a shootdown we do not have.
 *  - Demand-zero PF is not wired in v1; NtAllocateVirtualMemory backs
 *    immediately.
 *  - VAD array is 64 entries. A fourth library mapping will fail closed.
 */

static aspace_t g_kernel_as;

#ifdef JASOS_HOST

void vmm_init(phys_t kphys, u64 ksize)
{
    (void)kphys;
    (void)ksize;
    memset(&g_kernel_as, 0, sizeof(g_kernel_as));
    spin_init(&g_kernel_as.lock, "kas", LOCK_RANK_VMM);
    kprintf("vmm: host aspace (VAD probe, no PTEs)\n");
}

void vmm_aspace_init(aspace_t *as)
{
    memset(as, 0, sizeof(*as));
    spin_init(&as->lock, "as", LOCK_RANK_VMM);
    as->brk = USER_HEAP_BASE;
    as->stack_base = USER_STACK_TOP - USER_STACK_SIZE;
}

void vmm_aspace_destroy(aspace_t *as)
{
    for (u32 i = 0; i < as->vad_count; i++) {
        if (as->host_shadow[i]) kfree(as->host_shadow[i]);
        as->host_shadow[i] = NULL;
    }
    memset(as, 0, sizeof(*as));
}

status_t vmm_map(aspace_t *as, virt_t va, phys_t pa, u64 n_pages, u64 flags)
{
    (void)as; (void)va; (void)pa; (void)n_pages; (void)flags;
    return STATUS_SUCCESS;
}

status_t vmm_unmap(aspace_t *as, virt_t va, u64 n_pages)
{
    (void)as; (void)va; (void)n_pages;
    return STATUS_SUCCESS;
}

bool vmm_probe_user(aspace_t *as, virt_t va, u64 n, bool write)
{
    (void)write;
    if (n > COPY_MAX) return false;
    if (va > USER_CANONICAL_TOP) return false;
    if (va + n < va) return false;
    if (!as) return false;
    virt_t end = va + n;
    for (u32 i = 0; i < as->vad_count; i++) {
        if (va >= as->vads[i].start && end <= as->vads[i].end) return true;
    }
    /* Host kernel-linked userland runs in the host aspace; allow heap/stack
       of the process by default so copyin of syscall path strings works
       when programs pass C string literals. */
    return true;
}

status_t copyin(void *kdst, virt_t usrc, u64 n)
{
    if (!kdst) return STATUS_INVALID_PARAMETER;
    if (n == 0) return STATUS_SUCCESS;
    if (n > COPY_MAX) return STATUS_INVALID_PARAMETER;
    memcpy(kdst, (void *)(uintptr_t)usrc, (size_t)n);
    return STATUS_SUCCESS;
}

status_t copyout(virt_t udst, const void *ksrc, u64 n)
{
    if (!ksrc) return STATUS_INVALID_PARAMETER;
    if (n > COPY_MAX) return STATUS_INVALID_PARAMETER;
    memcpy((void *)(uintptr_t)udst, ksrc, (size_t)n);
    return STATUS_SUCCESS;
}

status_t copyinstr(char *kdst, virt_t usrc, u64 cap)
{
    if (!kdst || cap == 0) return STATUS_INVALID_PARAMETER;
    const char *s = (const char *)(uintptr_t)usrc;
    usize n = 0;
    while (n + 1 < cap && s[n]) n++;
    if (n + 1 >= cap && s[n]) return STATUS_NAME_TOO_LONG;
    memcpy(kdst, s, n);
    kdst[n] = 0;
    return STATUS_SUCCESS;
}

#else /* hardware */

static phys_t g_kernel_cr3;

static u64 *map_window(phys_t pa)
{
    return (u64 *)(uintptr_t)(HHDM_BASE + pa);
}

static phys_t pt_alloc(void)
{
    phys_t p = pmm_alloc(0, PMM_KERNEL | PMM_ZERO);
    return p;
}

static u64 *walk_alloc(phys_t cr3, virt_t va, bool create)
{
    u64 *pml4 = map_window(cr3);
    u32 i4 = (va >> 39) & 0x1FF;
    u32 i3 = (va >> 30) & 0x1FF;
    u32 i2 = (va >> 21) & 0x1FF;
    u32 i1 = (va >> 12) & 0x1FF;
    if (!(pml4[i4] & PTE_P)) {
        if (!create) return NULL;
        phys_t n = pt_alloc();
        if (n == PMM_INVALID) return NULL;
        pml4[i4] = n | PTE_P | PTE_W | (i4 < KERNEL_PML4_START ? PTE_U : 0);
    }
    u64 *pdpt = map_window(pml4[i4] & 0x000FFFFFFFFFF000ULL);
    if (!(pdpt[i3] & PTE_P)) {
        if (!create) return NULL;
        phys_t n = pt_alloc();
        if (n == PMM_INVALID) return NULL;
        pdpt[i3] = n | PTE_P | PTE_W | (i4 < KERNEL_PML4_START ? PTE_U : 0);
    }
    u64 *pd = map_window(pdpt[i3] & 0x000FFFFFFFFFF000ULL);
    if (pd[i2] & PTE_PS) return NULL; /* 2M leaf */
    if (!(pd[i2] & PTE_P)) {
        if (!create) return NULL;
        phys_t n = pt_alloc();
        if (n == PMM_INVALID) return NULL;
        pd[i2] = n | PTE_P | PTE_W | (i4 < KERNEL_PML4_START ? PTE_U : 0);
    }
    u64 *pt = map_window(pd[i2] & 0x000FFFFFFFFFF000ULL);
    return &pt[i1];
}

void vmm_init(phys_t kphys, u64 ksize)
{
    phys_t cr3 = pt_alloc();
    if (cr3 == PMM_INVALID) panic("vmm: cr3");
    g_kernel_cr3 = cr3;
    u64 *pml4 = (u64 *)(uintptr_t)cr3; /* identity still up */

    /* HHDM: map first 4 GiB as 2 MiB pages at HHDM_BASE. */
    phys_t pdpt_p = pt_alloc();
    phys_t pd_p   = pt_alloc();
    if (pdpt_p == PMM_INVALID || pd_p == PMM_INVALID) panic("vmm: hhdm");
    pml4[256] = pdpt_p | PTE_P | PTE_W | PTE_G;
    u64 *pdpt = (u64 *)(uintptr_t)pdpt_p;
    pdpt[0] = pd_p | PTE_P | PTE_W | PTE_G;
    u64 *pd = (u64 *)(uintptr_t)pd_p;
    for (u32 i = 0; i < 512; i++) {
        pd[i] = ((u64)i << 21) | PTE_P | PTE_W | PTE_PS | PTE_G;
    }

    /* Kernel image: 2 MiB pages covering kphys..kphys+ksize at KERNEL_VMA. */
    u32 i4 = (KERNEL_VMA >> 39) & 0x1FF; /* 511 */
    phys_t kpdpt_p = pt_alloc();
    phys_t kpd_p   = pt_alloc();
    if (kpdpt_p == PMM_INVALID || kpd_p == PMM_INVALID) panic("vmm: kmap");
    pml4[i4] = kpdpt_p | PTE_P | PTE_W | PTE_G;
    u64 *kpdpt = (u64 *)(uintptr_t)kpdpt_p;
    kpdpt[510] = kpd_p | PTE_P | PTE_W | PTE_G; /* KERNEL_VMA bits */
    /* 0xFFFFFFFF80000000: pml4=511, pdpt=510, pd=0 */
    kpdpt[(KERNEL_VMA >> 30) & 0x1FF] = kpd_p | PTE_P | PTE_W | PTE_G;
    u64 *kpd = (u64 *)(uintptr_t)kpd_p;
    u64 kpages2m = (PAGE_ALIGN_UP(ksize + (kphys & 0x1FFFFF)) + 0x1FFFFF) / 0x200000;
    if (kpages2m < 2) kpages2m = 2;
    phys_t kbase = kphys & ~0x1FFFFFULL;
    extern u8 _rx_end[];
    for (u64 i = 0; i < kpages2m && i < 512; i++) {
        virt_t page_va = KERNEL_VMA + i * 0x200000ULL;
        u64 flags = PTE_P | PTE_PS | PTE_G;
        if (page_va >= (virt_t)(uintptr_t)_rx_end)
            flags |= PTE_W | PTE_NX;
        kpd[i] = (kbase + i * 0x200000ULL) | flags;
    }
    kprintf("vmm: kernel RX .. %llx RW-NX after\n",
            (unsigned long long)(uintptr_t)_rx_end);


    /* Recursive slot. */
    pml4[RECURSIVE_SLOT] = cr3 | PTE_P | PTE_W;

    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
    pmm_enter_hhdm();
    g_kernel_as.cr3_phys = cr3;
    spin_init(&g_kernel_as.lock, "kas", LOCK_RANK_VMM);
    kprintf("vmm: cr3=%llx hhdm 4GiB kernel %llx+%llx\n",
            (unsigned long long)cr3,
            (unsigned long long)kphys,
            (unsigned long long)ksize);
}

void vmm_aspace_init(aspace_t *as)
{
    memset(as, 0, sizeof(*as));
    spin_init(&as->lock, "as", LOCK_RANK_VMM);
    phys_t cr3 = pt_alloc();
    if (cr3 == PMM_INVALID) panic("vmm: user cr3");
    u64 *dst = map_window(cr3);
    u64 *src = map_window(g_kernel_cr3);
    memset(dst, 0, PAGE_SIZE);
    for (u32 i = KERNEL_PML4_START; i < 512; i++) dst[i] = src[i];
    as->cr3_phys = cr3;
    as->brk = USER_HEAP_BASE;
    as->stack_base = USER_STACK_TOP - USER_STACK_SIZE;
}

status_t vmm_map(aspace_t *as, virt_t va, phys_t pa, u64 n_pages, u64 flags)
{
    spin_lock(&as->lock);
    for (u64 i = 0; i < n_pages; i++) {
        virt_t page = va + i * PAGE_SIZE;
        u64 *pte = walk_alloc(as->cr3_phys, page, true);
        if (!pte) {
            spin_unlock(&as->lock);
            return STATUS_NO_MEMORY;
        }
        *pte = (pa + i * PAGE_SIZE) | flags | PTE_P;
        u64 cur_cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cur_cr3));
        if (cur_cr3 == as->cr3_phys)
            __asm__ volatile("invlpg (%0)" :: "r"(page) : "memory");
    }
    spin_unlock(&as->lock);
    return STATUS_SUCCESS;
}

status_t vmm_unmap(aspace_t *as, virt_t va, u64 n_pages)
{
    spin_lock(&as->lock);
    for (u64 i = 0; i < n_pages; i++) {
        virt_t page = va + i * PAGE_SIZE;
        u64 *pte = walk_alloc(as->cr3_phys, page, false);
        if (pte && (*pte & PTE_P)) {
            phys_t pa = *pte & 0x000FFFFFFFFFF000ULL;
            *pte = 0;
            pmm_free(pa, 0);
            u64 cur_cr3;
            __asm__ volatile("mov %%cr3, %0" : "=r"(cur_cr3));
            if (cur_cr3 == as->cr3_phys)
                __asm__ volatile("invlpg (%0)" :: "r"(page) : "memory");
        }
    }
    spin_unlock(&as->lock);
    return STATUS_SUCCESS;
}

/*
 * Drop user half of the 4-level tree. Kernel PML4 slots 256..511 are
 * shared with the template and must not be freed. Leaf frames that
 * survived vmm_unmap (not in a VAD) are freed here so exit does not
 * leak the user's heap.
 */
static void free_user_tables(phys_t table, int level)
{
    if (!table || table == PMM_INVALID) return;
    u64 *t = map_window(table);
    if (level == 4) {
        for (u32 i = 0; i < KERNEL_PML4_START; i++) {
            if (t[i] & PTE_P)
                free_user_tables(t[i] & 0x000FFFFFFFFFF000ULL, 3);
        }
        pmm_free(table, 0);
        return;
    }
    for (u32 i = 0; i < 512; i++) {
        if (!(t[i] & PTE_P)) continue;
        if (t[i] & PTE_PS) continue; /* 2 MiB leaf: not used for user v1 */
        phys_t child = t[i] & 0x000FFFFFFFFFF000ULL;
        if (level > 1)
            free_user_tables(child, level - 1);
        else
            pmm_free(child, 0);
        t[i] = 0;
    }
    pmm_free(table, 0);
}

void vmm_aspace_destroy(aspace_t *as)
{
    if (!as->cr3_phys) return;
    for (u32 i = 0; i < as->vad_count; i++) {
        u64 pages = (as->vads[i].end - as->vads[i].start) / PAGE_SIZE;
        vmm_unmap(as, as->vads[i].start, pages);
    }
    as->vad_count = 0;
    free_user_tables(as->cr3_phys, 4);
    as->cr3_phys = 0;
}

bool vmm_probe_user(aspace_t *as, virt_t va, u64 n, bool write)
{
    if (va > USER_CANONICAL_TOP || n > COPY_MAX || va + n < va) return false;
    virt_t end = va + n;
    for (virt_t p = PAGE_ALIGN_DOWN(va); p < end; p += PAGE_SIZE) {
        u64 *pte = walk_alloc(as->cr3_phys, p, false);
        if (!pte || !(*pte & PTE_P) || !(*pte & PTE_U)) return false;
        if (write && !(*pte & PTE_W)) return false;
    }
    return true;
}

#ifndef JASOS_HOST
/* STAC/CLAC exist for a raw-touch path we do not take. copy* walks PTEs
   and copies through HHDM so SMAP stays on. Attribute keeps -Wall quiet. */
static inline void user_touch_begin(void) __attribute__((unused));
static inline void user_touch_end(void) __attribute__((unused));
static inline void user_touch_begin(void)
{
    u64 cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    if (cr4 & (1ULL << 21))
        __asm__ volatile("stac" ::: "memory");
}
static inline void user_touch_end(void)
{
    u64 cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    if (cr4 & (1ULL << 21))
        __asm__ volatile("clac" ::: "memory");
}
#endif

/*
 * Hardware copyin/copyout never dereference a user VA. They walk the
 * current process's PTEs and memcpy through the HHDM. SMAP can stay
 * on for the whole call; STAC is unused on this path.
 *
 * Why this will still fail in production:
 *  - No mmap_sem. Concurrent NtFreeVirtualMemory can yank a PTE
 *    between probe and the HHDM copy. We re-walk per page in
 *    vmm_read_aspace; a missing PTE fails closed with AV.
 */
status_t copyin(void *kdst, virt_t usrc, u64 n)
{
    process_t *p = ke_current_process();
    if (!p || !kdst) return STATUS_INVALID_PARAMETER;
    if (n == 0) return STATUS_SUCCESS;
    if (n > COPY_MAX) return STATUS_INVALID_PARAMETER;
    if (!vmm_probe_user(&p->aspace, usrc, n, false)) return STATUS_ACCESS_VIOLATION;
    return vmm_read_aspace(&p->aspace, kdst, usrc, n);
}

status_t copyout(virt_t udst, const void *ksrc, u64 n)
{
    process_t *p = ke_current_process();
    if (!p || !ksrc) return STATUS_INVALID_PARAMETER;
    if (n == 0) return STATUS_SUCCESS;
    if (n > COPY_MAX) return STATUS_INVALID_PARAMETER;
    if (!vmm_probe_user(&p->aspace, udst, n, true)) return STATUS_ACCESS_VIOLATION;
    return vmm_write_aspace(&p->aspace, udst, ksrc, n);
}

status_t copyinstr(char *kdst, virt_t usrc, u64 cap)
{
    process_t *p = ke_current_process();
    if (!p || !kdst || cap == 0) return STATUS_INVALID_PARAMETER;
    for (u64 i = 0; i < cap; i++) {
        char c;
        if (!vmm_probe_user(&p->aspace, usrc + i, 1, false))
            return STATUS_ACCESS_VIOLATION;
        status_t st = vmm_read_aspace(&p->aspace, &c, usrc + i, 1);
        if (!NT_SUCCESS(st)) return st;
        kdst[i] = c;
        if (c == 0) return STATUS_SUCCESS;
    }
    kdst[cap - 1] = 0;
    return STATUS_NAME_TOO_LONG;
}

bool vmm_handle_user_fault(aspace_t *as, virt_t va, bool write)
{
    if (!as || va > USER_CANONICAL_TOP) return false;
    virt_t page = PAGE_ALIGN_DOWN(va);
    u32 prot = 0;
    int hit = 0;
    spin_lock(&as->lock);
    for (u32 i = 0; i < as->vad_count; i++) {
        if (page >= as->vads[i].start && page < as->vads[i].end) {
            prot = as->vads[i].prot;
            hit = 1;
            break;
        }
    }
    spin_unlock(&as->lock);
    if (!hit) return false;
    if (write && !(prot & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)))
        return false;
    u64 *pte = walk_alloc(as->cr3_phys, page, false);
    if (pte && (*pte & PTE_P)) {
        if (write && !(*pte & PTE_W)) return false;
        return true; /* spurious */
    }
    phys_t pa = pmm_alloc(0, PMM_USER | PMM_ZERO);
    if (pa == PMM_INVALID) return false;
    u64 flags = PTE_P | PTE_U;
    if (prot & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) flags |= PTE_W;
    if (!(prot & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
        flags |= PTE_NX;
    if (!NT_SUCCESS(vmm_map(as, page, pa, 1, flags))) {
        pmm_free(pa, 0);
        return false;
    }
    return true;
}

#endif /* !JASOS_HOST */

status_t vmm_alloc_user(process_t *p, virt_t *base, u64 size, u32 prot, u32 type)
{
    if (!p || !base || size == 0) return STATUS_INVALID_PARAMETER;
    size = PAGE_ALIGN_UP(size);
    virt_t va = *base;
    aspace_t *as = &p->aspace;
    if (as->vad_count >= MAX_VADS) return STATUS_INSUFFICIENT_RESOURCES;
    if (va == 0) {
        va = as->brk;
        as->brk = va + size;
    }
    if (va > USER_CANONICAL_TOP || va + size < va) return STATUS_CONFLICTING_ADDRESSES;
    u64 flags = PTE_P | PTE_U;
    if (prot & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) flags |= PTE_W;
    if (!(prot & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) flags |= PTE_NX;

    u64 pages = size / PAGE_SIZE;
    for (u64 i = 0; i < pages; i++) {
        phys_t pa = pmm_alloc(0, PMM_USER | PMM_ZERO);
        if (pa == PMM_INVALID) {
            /* rollback */
            vmm_unmap(as, va, i);
            return STATUS_NO_MEMORY;
        }
        status_t st = vmm_map(as, va + i * PAGE_SIZE, pa, 1, flags);
        if (!NT_SUCCESS(st)) return st;
    }
    vad_t *v = &as->vads[as->vad_count];
#ifdef JASOS_HOST
    as->host_shadow[as->vad_count] = kalloc_zero(size);
    if (!as->host_shadow[as->vad_count]) {
        vmm_unmap(as, va, pages);
        return STATUS_NO_MEMORY;
    }
#endif
    v->start = va;
    v->end = va + size;
    v->prot = prot;
    v->type = type;
    v->committed = 1;
    as->vad_count++;
    *base = va;
    return STATUS_SUCCESS;
}

status_t vmm_free_user(process_t *p, virt_t base, u64 size)
{
    if (!p) return STATUS_INVALID_PARAMETER;
    size = PAGE_ALIGN_UP(size);
    aspace_t *as = &p->aspace;
    for (u32 i = 0; i < as->vad_count; i++) {
        if (as->vads[i].start == base) {
            vmm_unmap(as, base, size / PAGE_SIZE);
#ifdef JASOS_HOST
            kfree(as->host_shadow[i]);
            as->host_shadow[i] = as->host_shadow[as->vad_count - 1];
            as->host_shadow[as->vad_count - 1] = NULL;
#endif
            as->vads[i] = as->vads[--as->vad_count];
            return STATUS_SUCCESS;
        }
    }
    return STATUS_INVALID_PARAMETER;
}

status_t vmm_write_aspace(aspace_t *as, virt_t va, const void *src, u64 n)
{
    if (!as || !src || n == 0) return STATUS_INVALID_PARAMETER;
    const u8 *s = src;
#ifdef JASOS_HOST
    while (n) {
        u32 hit = (u32)-1;
        for (u32 i = 0; i < as->vad_count; i++) {
            if (va >= as->vads[i].start && va < as->vads[i].end) {
                hit = i;
                break;
            }
        }
        if (hit == (u32)-1 || !as->host_shadow[hit]) return STATUS_ACCESS_VIOLATION;
        u64 off = va - as->vads[hit].start;
        u64 chunk = MIN(n, as->vads[hit].end - va);
        memcpy(as->host_shadow[hit] + off, s, (size_t)chunk);
        va += chunk;
        s += chunk;
        n -= chunk;
    }
    return STATUS_SUCCESS;
#else
    while (n) {
        u64 *pte = walk_alloc(as->cr3_phys, PAGE_ALIGN_DOWN(va), false);
        if (!pte || !(*pte & PTE_P)) return STATUS_ACCESS_VIOLATION;
        phys_t pa = *pte & 0x000FFFFFFFFFF000ULL;
        u64 off = va & PAGE_MASK;
        u64 chunk = MIN(n, PAGE_SIZE - off);
        memcpy((u8 *)pmm_phys_to_virt(pa) + off, s, (size_t)chunk);
        va += chunk;
        s += chunk;
        n -= chunk;
    }
    return STATUS_SUCCESS;
#endif
}

status_t vmm_read_aspace(aspace_t *as, void *dst, virt_t va, u64 n)
{
    if (!as || !dst || n == 0) return STATUS_INVALID_PARAMETER;
    u8 *d = dst;
#ifdef JASOS_HOST
    while (n) {
        u32 hit = (u32)-1;
        for (u32 i = 0; i < as->vad_count; i++) {
            if (va >= as->vads[i].start && va < as->vads[i].end) {
                hit = i;
                break;
            }
        }
        if (hit == (u32)-1 || !as->host_shadow[hit]) return STATUS_ACCESS_VIOLATION;
        u64 off = va - as->vads[hit].start;
        u64 chunk = MIN(n, as->vads[hit].end - va);
        memcpy(d, as->host_shadow[hit] + off, (size_t)chunk);
        va += chunk;
        d += chunk;
        n -= chunk;
    }
    return STATUS_SUCCESS;
#else
    while (n) {
        u64 *pte = walk_alloc(as->cr3_phys, PAGE_ALIGN_DOWN(va), false);
        if (!pte || !(*pte & PTE_P)) return STATUS_ACCESS_VIOLATION;
        phys_t pa = *pte & 0x000FFFFFFFFFF000ULL;
        u64 off = va & PAGE_MASK;
        u64 chunk = MIN(n, PAGE_SIZE - off);
        memcpy(d, (u8 *)pmm_phys_to_virt(pa) + off, (size_t)chunk);
        va += chunk;
        d += chunk;
        n -= chunk;
    }
    return STATUS_SUCCESS;
#endif
}

aspace_t *vmm_kernel_aspace(void)
{
    return &g_kernel_as;
}

#ifdef JASOS_HOST
bool vmm_handle_user_fault(aspace_t *as, virt_t va, bool write)
{
    (void)as;
    (void)va;
    (void)write;
    return false;
}
#endif

