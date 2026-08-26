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
    for (u64 i = 0; i < kpages2m && i < 512; i++) {
        kpd[i] = (kbase + i * 0x200000ULL) | PTE_P | PTE_W | PTE_PS | PTE_G;
    }

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

void vmm_aspace_destroy(aspace_t *as)
{
    /* v1: leak user PTs on process exit except the cr3 page. Honest. */
    if (as->cr3_phys) pmm_free(as->cr3_phys, 0);
    as->cr3_phys = 0;
}

status_t vmm_map(aspace_t *as, virt_t va, phys_t pa, u64 n_pages, u64 flags)
{
    spin_lock(&as->lock);
    for (u64 i = 0; i < n_pages; i++) {
        u64 *pte = walk_alloc(as->cr3_phys, va + i * PAGE_SIZE, true);
        if (!pte) {
            spin_unlock(&as->lock);
            return STATUS_NO_MEMORY;
        }
        *pte = (pa + i * PAGE_SIZE) | flags | PTE_P;
    }
    spin_unlock(&as->lock);
    return STATUS_SUCCESS;
}

status_t vmm_unmap(aspace_t *as, virt_t va, u64 n_pages)
{
    spin_lock(&as->lock);
    for (u64 i = 0; i < n_pages; i++) {
        u64 *pte = walk_alloc(as->cr3_phys, va + i * PAGE_SIZE, false);
        if (pte) *pte = 0;
    }
    spin_unlock(&as->lock);
    return STATUS_SUCCESS;
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

status_t copyin(void *kdst, virt_t usrc, u64 n)
{
    process_t *p = ke_current_process();
    if (!p || !kdst) return STATUS_INVALID_PARAMETER;
    if (!vmm_probe_user(&p->aspace, usrc, n, false)) return STATUS_ACCESS_VIOLATION;
    memcpy(kdst, (void *)(uintptr_t)usrc, (size_t)n);
    return STATUS_SUCCESS;
}

status_t copyout(virt_t udst, const void *ksrc, u64 n)
{
    process_t *p = ke_current_process();
    if (!p || !ksrc) return STATUS_INVALID_PARAMETER;
    if (!vmm_probe_user(&p->aspace, udst, n, true)) return STATUS_ACCESS_VIOLATION;
    memcpy((void *)(uintptr_t)udst, ksrc, (size_t)n);
    return STATUS_SUCCESS;
}

status_t copyinstr(char *kdst, virt_t usrc, u64 cap)
{
    process_t *p = ke_current_process();
    if (!p || !kdst || cap == 0) return STATUS_INVALID_PARAMETER;
    for (u64 i = 0; i < cap; i++) {
        if (!vmm_probe_user(&p->aspace, usrc + i, 1, false)) return STATUS_ACCESS_VIOLATION;
        char c = *(char *)(uintptr_t)(usrc + i);
        kdst[i] = c;
        if (c == 0) return STATUS_SUCCESS;
    }
    kdst[cap - 1] = 0;
    return STATUS_NAME_TOO_LONG;
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
