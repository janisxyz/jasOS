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
 *  - Kernel map is frozen after vmm_init except kstack leaves under a
 *    pre-created KERNEL_STACK_BASE PDPT. Loadable drivers still cannot
 *    add a new kernel PML4 slot without a shootdown we do not have.
 *  - Demand-zero fills one page at a time on PF / copyin. A 32 MiB
 *    commit that is then touched serially still costs 8192 pmm_allocs
 *    on the fault path, not at NtAllocate.
 *  - VAD array is 64 entries. A mapping bomb fails closed on VAD count
 *    or USER_COMMIT_MAX (32 MiB), whichever hits first.
 */

static aspace_t g_kernel_as;

static int vad_lookup(aspace_t *as, virt_t page)
{
    if (!as) return -1;
    for (u32 i = 0; i < as->vad_count; i++) {
        if (page >= as->vads[i].start && page < as->vads[i].end)
            return (int)i;
    }
    return -1;
}

#ifndef JASOS_HOST
static u64 pte_flags_from_prot(u32 prot)
{
    u64 flags = PTE_P | PTE_U;
    if (prot & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) flags |= PTE_W;
    if (!(prot & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
        flags |= PTE_NX;
    return flags;
}
#endif

static status_t vmm_populate_page(aspace_t *as, virt_t page, bool write);

#ifdef JASOS_HOST

void vmm_init(phys_t kphys, u64 ksize)
{
    (void)kphys;
    (void)ksize;
    memset(&g_kernel_as, 0, sizeof(g_kernel_as));
    spin_init(&g_kernel_as.lock, "kas", LOCK_RANK_VMM);
    kprintf("vmm: host aspace (VAD probe, demand-zero, no PTEs)\n");
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
    if (n > COPY_MAX) return false;
    if (va > USER_CANONICAL_TOP) return false;
    if (va + n < va) return false;
    if (!as) return false;
    virt_t end = va + n;
    int any = 0;
    for (virt_t p = PAGE_ALIGN_DOWN(va); p < end; p += PAGE_SIZE) {
        int idx = vad_lookup(as, p);
        if (idx < 0) {
            /* Host kernel-linked userland runs in the host aspace; allow
               C string literals when no VAD covers them. */
            continue;
        }
        any = 1;
        u32 prot = as->vads[idx].prot;
        if (write && !(prot & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)))
            return false;
    }
    if (any) return true;
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

status_t vmm_map_kstack(u32 tid, u8 **out)
{
    (void)tid;
    if (!out) return STATUS_INVALID_PARAMETER;
    *out = NULL;
    return STATUS_NOT_SUPPORTED;
}

void vmm_unmap_kstack(u32 tid)
{
    (void)tid;
}

status_t vmm_map_guarded_stack(virt_t base, u64 size, u8 **out)
{
    (void)base;
    (void)size;
    if (!out) return STATUS_INVALID_PARAMETER;
    *out = NULL;
    return STATUS_NOT_SUPPORTED;
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

/*
 * Fill a 4 KiB PT covering `base`..`base+2MiB`. Kernel RX physical
 * pages are present + NX + not writable. Everything else is RW+NX.
 * Identity still live; `pt` is a physical pointer.
 */
static void fill_lo_pt(u64 *pt, phys_t base, phys_t rx_lo, phys_t rx_hi)
{
    for (u32 i = 0; i < 512; i++) {
        phys_t pa = base + (phys_t)i * PAGE_SIZE;
        u64 f = PTE_P | PTE_G | PTE_NX;
        if (pa < rx_lo || pa >= rx_hi)
            f |= PTE_W;
        pt[i] = pa | f;
    }
}

static void cpu_enable_nxe(void)
{
    u32 a, d;
    __asm__ volatile("rdmsr" : "=a"(a), "=d"(d) : "c"(0xC0000080u));
    a |= (1u << 11); /* IA32_EFER.NXE */
    __asm__ volatile("wrmsr" :: "c"(0xC0000080u), "a"(a), "d"(d));
}

void vmm_init(phys_t kphys, u64 ksize)
{
    cpu_enable_nxe();

    phys_t cr3 = pt_alloc();
    if (cr3 == PMM_INVALID) panic("vmm: cr3");
    g_kernel_cr3 = cr3;
    u64 *pml4 = (u64 *)(uintptr_t)cr3; /* identity still up */

    extern u8 _rx_end[];
    phys_t rx_lo = PAGE_ALIGN_DOWN(kphys);
    phys_t rx_hi = PAGE_ALIGN_UP((phys_t)((uintptr_t)_rx_end - KERNEL_VMA));

    /* HHDM: map first 4 GiB as 2 MiB pages at HHDM_BASE, all NX.
       The 2 MiB page(s) covering kernel RX are split to 4 KiB and
       stripped of PTE_W so a kernel bug writing through HHDM cannot
       clobber .text. VGA (0xB8000) and the boot stack stay RW. */
    phys_t pdpt_p = pt_alloc();
    phys_t pd_p   = pt_alloc();
    if (pdpt_p == PMM_INVALID || pd_p == PMM_INVALID) panic("vmm: hhdm");
    pml4[256] = pdpt_p | PTE_P | PTE_W | PTE_G;
    u64 *pdpt = (u64 *)(uintptr_t)pdpt_p;
    pdpt[0] = pd_p | PTE_P | PTE_W | PTE_G;
    u64 *pd = (u64 *)(uintptr_t)pd_p;
    for (u32 i = 0; i < 512; i++) {
        pd[i] = ((u64)i << 21) | PTE_P | PTE_W | PTE_PS | PTE_G | PTE_NX;
    }
    for (phys_t base = rx_lo & ~0x1FFFFFULL; base < rx_hi; base += 0x200000ULL) {
        u32 i2 = (u32)((base >> 21) & 0x1FF);
        phys_t ptp = pt_alloc();
        if (ptp == PMM_INVALID) panic("vmm: hhdm split");
        fill_lo_pt((u64 *)(uintptr_t)ptp, base, rx_lo, rx_hi);
        pd[i2] = ptp | PTE_P | PTE_W | PTE_G;
    }

    /* Kernel-CR3-only identity of the first 2 MiB so the boot stack
       and VGA survive the CR3 switch. User CR3 copies skip PML4[0]. */
    {
        phys_t id_pdpt = pt_alloc();
        phys_t id_pd   = pt_alloc();
        phys_t id_pt   = pt_alloc();
        if (id_pdpt == PMM_INVALID || id_pd == PMM_INVALID || id_pt == PMM_INVALID)
            panic("vmm: ident");
        pml4[0] = id_pdpt | PTE_P | PTE_W | PTE_G;
        u64 *idp = (u64 *)(uintptr_t)id_pdpt;
        idp[0] = id_pd | PTE_P | PTE_W | PTE_G;
        u64 *idd = (u64 *)(uintptr_t)id_pd;
        idd[0] = id_pt | PTE_P | PTE_W | PTE_G;
        fill_lo_pt((u64 *)(uintptr_t)id_pt, 0, rx_lo, rx_hi);
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
        virt_t page_va = KERNEL_VMA + i * 0x200000ULL;
        u64 flags = PTE_P | PTE_PS | PTE_G;
        if (page_va >= (virt_t)(uintptr_t)_rx_end)
            flags |= PTE_W | PTE_NX;
        kpd[i] = (kbase + i * 0x200000ULL) | flags;
    }
    kprintf("vmm: kernel RX .. %llx RW-NX after; HHDM NX; phys [%llx,%llx) RO\n",
            (unsigned long long)(uintptr_t)_rx_end,
            (unsigned long long)rx_lo,
            (unsigned long long)rx_hi);

    /* Recursive slot: writable (PTE updates) but NX (do not execute tables). */
    pml4[RECURSIVE_SLOT] = cr3 | PTE_P | PTE_W | PTE_NX;

    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
    pmm_enter_hhdm();
    serial_use_hhdm();
    g_kernel_as.cr3_phys = cr3;
    spin_init(&g_kernel_as.lock, "kas", LOCK_RANK_VMM);

    /* Pre-create KERNEL_STACK_BASE tables so every user CR3 that
       copies the kernel half shares the PDPT. Leaves stay not-present
       until vmm_map_kstack. */
    if (!walk_alloc(cr3, KERNEL_STACK_BASE, true))
        panic("vmm: kstack slot");
    if (!walk_alloc(cr3, IST_STACK_BASE, true))
        panic("vmm: ist slot");

    kprintf("vmm: cr3=%llx hhdm 4GiB kernel %llx+%llx kstack %llx\n",
            (unsigned long long)cr3,
            (unsigned long long)kphys,
            (unsigned long long)ksize,
            (unsigned long long)KERNEL_STACK_BASE);
}

status_t vmm_map_guarded_stack(virt_t base, u64 size, u8 **out)
{
    if (!out || !base || size == 0 || (size & PAGE_MASK))
        return STATUS_INVALID_PARAMETER;
    u64 np = size / PAGE_SIZE;
    for (u64 i = 0; i < np; i++) {
        phys_t pa = pmm_alloc(0, PMM_KERNEL | PMM_ZERO);
        if (pa == PMM_INVALID) {
            vmm_unmap(&g_kernel_as, base + PAGE_SIZE, i);
            return STATUS_NO_MEMORY;
        }
        status_t st = vmm_map(&g_kernel_as, base + PAGE_SIZE + i * PAGE_SIZE, pa, 1,
                              PTE_P | PTE_W | PTE_G | PTE_NX);
        if (!NT_SUCCESS(st)) {
            pmm_free(pa, 0);
            vmm_unmap(&g_kernel_as, base + PAGE_SIZE, i);
            return st;
        }
    }
    *out = (u8 *)(uintptr_t)(base + PAGE_SIZE);
    return STATUS_SUCCESS;
}

status_t vmm_map_kstack(u32 tid, u8 **out)
{
    if (!out || tid == 0 || tid > MAX_THREADS) return STATUS_INVALID_PARAMETER;
    virt_t base = KERNEL_STACK_BASE + (u64)(tid - 1) * KSTACK_STRIDE;
    return vmm_map_guarded_stack(base, KSTACK_SIZE, out);
}

void vmm_unmap_kstack(u32 tid)
{
    if (tid == 0 || tid > MAX_THREADS) return;
    virt_t base = KERNEL_STACK_BASE + (u64)(tid - 1) * KSTACK_STRIDE;
    vmm_unmap(&g_kernel_as, base + PAGE_SIZE, KSTACK_SIZE / PAGE_SIZE);
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
        } else if (pte) {
            *pte = 0; /* proto PTE_SW_COMMIT, no frame */
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
    if (!as) return false;
    virt_t end = va + n;
    for (virt_t p = PAGE_ALIGN_DOWN(va); p < end; p += PAGE_SIZE) {
        int idx = vad_lookup(as, p);
        if (idx < 0) return false;
        u32 prot = as->vads[idx].prot;
        if (write) {
            if (!(prot & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)))
                return false;
        } else {
            if (!(prot & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE |
                          PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
                return false;
        }
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

#endif /* !JASOS_HOST */

static status_t vmm_populate_page(aspace_t *as, virt_t page, bool write)
{
    int idx = vad_lookup(as, page);
    if (idx < 0) return STATUS_ACCESS_VIOLATION;
    u32 prot = as->vads[idx].prot;
    if (write && !(prot & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)))
        return STATUS_ACCESS_VIOLATION;
#ifdef JASOS_HOST
    if (!as->host_shadow[idx]) {
        u64 sz = as->vads[idx].end - as->vads[idx].start;
        as->host_shadow[idx] = kalloc_zero(sz);
        if (!as->host_shadow[idx]) return STATUS_NO_MEMORY;
    }
    return STATUS_SUCCESS;
#else
    u64 *pte = walk_alloc(as->cr3_phys, page, false);
    if (pte && (*pte & PTE_P)) {
        if (write && !(*pte & PTE_W)) return STATUS_ACCESS_VIOLATION;
        return STATUS_SUCCESS;
    }
    phys_t pa = pmm_alloc(0, PMM_USER | PMM_ZERO);
    if (pa == PMM_INVALID) return STATUS_NO_MEMORY;
    status_t st = vmm_map(as, page, pa, 1, pte_flags_from_prot(prot));
    if (!NT_SUCCESS(st)) {
        pmm_free(pa, 0);
        return st;
    }
    return STATUS_SUCCESS;
#endif
}

bool vmm_handle_user_fault(aspace_t *as, virt_t va, bool write)
{
    if (!as || va > USER_CANONICAL_TOP) return false;
    return NT_SUCCESS(vmm_populate_page(as, PAGE_ALIGN_DOWN(va), write));
}

status_t vmm_alloc_user(process_t *p, virt_t *base, u64 size, u32 prot, u32 type)
{
    if (!p || !base || size == 0) return STATUS_INVALID_PARAMETER;
    size = PAGE_ALIGN_UP(size);
    aspace_t *as = &p->aspace;
    virt_t va = *base;
    u64 pages = size / PAGE_SIZE;
    if (pages > (USER_COMMIT_MAX / PAGE_SIZE))
        return STATUS_INSUFFICIENT_RESOURCES;

    spin_lock(&as->lock);
    if (va == 0)
        va = as->brk;
    if (va > USER_CANONICAL_TOP || va + size < va ||
        (size && va + size - 1 > USER_CANONICAL_TOP)) {
        spin_unlock(&as->lock);
        return STATUS_CONFLICTING_ADDRESSES;
    }
    if (as->vad_count >= MAX_VADS) {
        spin_unlock(&as->lock);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (as->committed_pages + pages > (USER_COMMIT_MAX / PAGE_SIZE)) {
        spin_unlock(&as->lock);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    for (u32 i = 0; i < as->vad_count; i++) {
        if (va < as->vads[i].end && va + size > as->vads[i].start) {
            spin_unlock(&as->lock);
            return STATUS_CONFLICTING_ADDRESSES;
        }
    }
    u32 idx = as->vad_count;
    vad_t *v = &as->vads[idx];
    v->start = va;
    v->end = va + size;
    v->prot = prot;
    v->type = type;
    v->committed = 1;
#ifdef JASOS_HOST
    as->host_shadow[idx] = NULL;
#endif
    as->vad_count++;
    as->committed_pages += pages;
    if (*base == 0)
        as->brk = va + size;
    spin_unlock(&as->lock);

#ifndef JASOS_HOST
    for (u64 i = 0; i < pages; i++) {
        u64 *pte = walk_alloc(as->cr3_phys, va + i * PAGE_SIZE, true);
        if (!pte) {
            vmm_free_user(p, va, size);
            return STATUS_NO_MEMORY;
        }
        *pte = PTE_SW_COMMIT;
    }
#endif
    *base = va;
    (void)type;
    return STATUS_SUCCESS;
}

status_t vmm_free_user(process_t *p, virt_t base, u64 size)
{
    if (!p) return STATUS_INVALID_PARAMETER;
    (void)size;
    aspace_t *as = &p->aspace;
    for (u32 i = 0; i < as->vad_count; i++) {
        if (as->vads[i].start == base) {
            u64 pages = (as->vads[i].end - as->vads[i].start) / PAGE_SIZE;
            vmm_unmap(as, as->vads[i].start, pages);
#ifdef JASOS_HOST
            kfree(as->host_shadow[i]);
            as->host_shadow[i] = as->host_shadow[as->vad_count - 1];
            as->host_shadow[as->vad_count - 1] = NULL;
#endif
            if (as->committed_pages >= pages)
                as->committed_pages -= pages;
            else
                as->committed_pages = 0;
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
        status_t st = vmm_populate_page(as, PAGE_ALIGN_DOWN(va), false);
        if (!NT_SUCCESS(st)) return st;
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
        status_t st = vmm_populate_page(as, PAGE_ALIGN_DOWN(va), false);
        if (!NT_SUCCESS(st)) return st;
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
        status_t st = vmm_populate_page(as, PAGE_ALIGN_DOWN(va), false);
        if (!NT_SUCCESS(st)) return st;
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
        status_t st = vmm_populate_page(as, PAGE_ALIGN_DOWN(va), false);
        if (!NT_SUCCESS(st)) return st;
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

