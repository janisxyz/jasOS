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
 *  - Protect/free of a range that spans two already-split VADs with a
 *    hole between them is CONFLICTING. Coalesce only joins adjacent
 *    same-prot VADs after protect; it does not walk a range of mixed
 *    VADs in one syscall.
 *  - T15: unmap/free collect frames under VMM then pmm_free after drop.
 *    OOM during a >16-page free falls back to 16-page batches; a
 *    populate of an already-cleared page in that window leaks one frame.
 */

static aspace_t g_kernel_as;

#ifndef JASOS_HOST
static void apply_prot_range(aspace_t *as, virt_t base, u64 size, u32 prot);
#endif

static int vad_lookup(aspace_t *as, virt_t page)
{
    if (!as) return -1;
    for (u32 i = 0; i < as->vad_count; i++) {
        if (page >= as->vads[i].start && page < as->vads[i].end)
            return (int)i;
    }
    return -1;
}

/* Unique VAD that fully contains [base, end). -1 if none or empty. */
static int vad_contains_range(aspace_t *as, virt_t base, virt_t end)
{
    if (!as || end <= base) return -1;
    for (u32 i = 0; i < as->vad_count; i++) {
        if (base >= as->vads[i].start && end <= as->vads[i].end)
            return (int)i;
    }
    return -1;
}

/* T24: [base,end) covered by a hole-free run of VADs, mixed prot
 * allowed. Returns the VAD index that contains `base`, or -1.
 * A hole anywhere in the range is -1. */
static int vad_run_covers(aspace_t *as, virt_t base, virt_t end, u32 *n_out)
{
    if (!as || end <= base) return -1;
    int first = vad_lookup(as, PAGE_ALIGN_DOWN(base));
    if (first < 0) return -1;
    u32 n = 0;
    virt_t cur = base;
    while (cur < end) {
        int idx = vad_lookup(as, cur);
        if (idx < 0) return -1;
        if (as->vads[idx].end <= cur) return -1;
        n++;
        cur = as->vads[idx].end;
        if (n > MAX_VADS) return -1;
    }
    if (n_out) *n_out = n;
    return first;
}

static u32 vad_append(aspace_t *as, virt_t start, virt_t end, u32 prot, u32 type, u32 committed)
{
    u32 i = as->vad_count++;
    as->vads[i].start = start;
    as->vads[i].end = end;
    as->vads[i].prot = prot;
    as->vads[i].type = type;
    as->vads[i].committed = committed;
#ifdef JASOS_HOST
    as->host_pages[i] = NULL;
    as->host_npages[i] = (u32)((end - start) / PAGE_SIZE);
#endif
    return i;
}

static void vad_remove_at(aspace_t *as, u32 i)
{
#ifdef JASOS_HOST
    as->host_pages[i] = as->host_pages[as->vad_count - 1];
    as->host_npages[i] = as->host_npages[as->vad_count - 1];
    as->host_pages[as->vad_count - 1] = NULL;
    as->host_npages[as->vad_count - 1] = 0;
#endif
    as->vads[i] = as->vads[--as->vad_count];
}

/*
 * Collapse adjacent VADs with identical prot/type. Without this, a
 * middle protect then a restore leaves three RW VADs and the original
 * range can no longer be named in one NtProtect. Touched host shadows
 * concatenate; kalloc failure leaves them split (not a security fail).
 */
static void vad_coalesce(aspace_t *as)
{
    for (;;) {
        int keep = -1, eat = -1;
        u32 nk = 0, ne = 0;
        spin_lock(&as->lock);
        for (u32 i = 0; i < as->vad_count && keep < 0; i++) {
            for (u32 j = 0; j < as->vad_count; j++) {
                if (i == j) continue;
                if (as->vads[i].end == as->vads[j].start &&
                    as->vads[i].prot == as->vads[j].prot &&
                    as->vads[i].type == as->vads[j].type &&
                    as->vads[i].committed == as->vads[j].committed) {
                    keep = (int)i;
                    eat = (int)j;
                    nk = (u32)((as->vads[i].end - as->vads[i].start) / PAGE_SIZE);
                    ne = (u32)((as->vads[j].end - as->vads[j].start) / PAGE_SIZE);
                    break;
                }
            }
        }
        if (keep < 0) {
            spin_unlock(&as->lock);
            return;
        }
#ifdef JASOS_HOST
        int need = (as->host_pages[keep] != NULL) || (as->host_pages[eat] != NULL);
        u8 **old_k = as->host_pages[keep];
        u8 **old_e = as->host_pages[eat];
        spin_unlock(&as->lock);
        u8 **comb = NULL;
        if (need) {
            comb = kalloc_zero((nk + ne) * sizeof(u8 *));
            if (!comb) return;
        }
        spin_lock(&as->lock);
        if (keep >= (int)as->vad_count || eat >= (int)as->vad_count ||
            as->vads[keep].end != as->vads[eat].start ||
            as->vads[keep].prot != as->vads[eat].prot) {
            spin_unlock(&as->lock);
            kfree(comb);
            return;
        }
        if (comb) {
            u8 **hk = as->host_pages[keep];
            u8 **he = as->host_pages[eat];
            for (u32 p = 0; p < nk; p++)
                comb[p] = hk ? hk[p] : NULL;
            for (u32 p = 0; p < ne; p++)
                comb[nk + p] = he ? he[p] : NULL;
            as->host_pages[keep] = comb;
            as->host_npages[keep] = nk + ne;
            as->host_pages[eat] = NULL;
            as->host_npages[eat] = 0;
        }
        as->vads[keep].end = as->vads[eat].end;
        vad_remove_at(as, (u32)eat);
        spin_unlock(&as->lock);
        if (old_k && old_k != comb) kfree(old_k);
        if (old_e && old_e != comb) kfree(old_e);
#else
        as->vads[keep].end = as->vads[eat].end;
        vad_remove_at(as, (u32)eat);
        spin_unlock(&as->lock);
        (void)nk;
        (void)ne;
#endif
    }
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

static void host_pages_free(aspace_t *as, u32 i)
{
    if (!as->host_pages[i]) {
        as->host_npages[i] = 0;
        return;
    }
    for (u32 p = 0; p < as->host_npages[i]; p++) {
        if (as->host_pages[i][p]) kfree(as->host_pages[i][p]);
        as->host_pages[i][p] = NULL;
    }
    kfree(as->host_pages[i]);
    as->host_pages[i] = NULL;
    as->host_npages[i] = 0;
}

void vmm_init(phys_t kphys, u64 ksize)
{
    (void)kphys;
    (void)ksize;
    memset(&g_kernel_as, 0, sizeof(g_kernel_as));
    spin_init(&g_kernel_as.lock, "kas", LOCK_RANK_VMM);
    kprintf("vmm: host aspace (VAD probe, per-page shadow, no PTEs)\n");
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
    for (u32 i = 0; i < as->vad_count; i++)
        host_pages_free(as, i);
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
        if (prot & PAGE_NOACCESS) return false;
        if (write && !(prot & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)))
            return false;
        if (!write && !(prot & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE |
                                PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
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

#define PTE_ADDR     0x000FFFFFFFFFF000ULL
#define UNMAP_BATCH  16u

static u64 *walk_alloc(phys_t cr3, virt_t va, bool create)
{
    u64 *pml4 = map_window(cr3);
    u32 i4 = (va >> 39) & 0x1FF;
    u32 i3 = (va >> 30) & 0x1FF;
    u32 i1 = (va >> 12) & 0x1FF;
    u32 i2 = (va >> 21) & 0x1FF;
    u64 u = (i4 < KERNEL_PML4_START) ? PTE_U : 0;
    if (!(pml4[i4] & PTE_P)) {
        if (!create) return NULL;
        phys_t n = pt_alloc();
        if (n == PMM_INVALID) return NULL;
        pml4[i4] = n | PTE_P | PTE_W | u;
    }
    u64 *pdpt = map_window(pml4[i4] & PTE_ADDR);
    if (!(pdpt[i3] & PTE_P)) {
        if (!create) return NULL;
        phys_t n = pt_alloc();
        if (n == PMM_INVALID) return NULL;
        pdpt[i3] = n | PTE_P | PTE_W | u;
    }
    u64 *pd = map_window(pdpt[i3] & PTE_ADDR);
    if (pd[i2] & PTE_PS) return NULL; /* 2M leaf */
    if (!(pd[i2] & PTE_P)) {
        if (!create) return NULL;
        phys_t n = pt_alloc();
        if (n == PMM_INVALID) return NULL;
        pd[i2] = n | PTE_P | PTE_W | u;
    }
    u64 *pt = map_window(pd[i2] & PTE_ADDR);
    return &pt[i1];
}

/*
 * Boot-only walker may pt_alloc (vmm_init holds no VMM lock).
 * Post-boot map/populate must not: PMM is rank 2, VMM is 4.
 * walk_fill_one installs one preallocated table at the first hole.
 */
static int walk_fill_one(phys_t cr3, virt_t va, phys_t n)
{
    u64 *pml4 = map_window(cr3);
    u32 i4 = (va >> 39) & 0x1FF;
    u32 i3 = (va >> 30) & 0x1FF;
    u32 i2 = (va >> 21) & 0x1FF;
    u64 u = (i4 < KERNEL_PML4_START) ? PTE_U : 0;
    if (!(pml4[i4] & PTE_P)) {
        pml4[i4] = n | PTE_P | PTE_W | u;
        return 1;
    }
    u64 *pdpt = map_window(pml4[i4] & PTE_ADDR);
    if (!(pdpt[i3] & PTE_P)) {
        pdpt[i3] = n | PTE_P | PTE_W | u;
        return 1;
    }
    u64 *pd = map_window(pdpt[i3] & PTE_ADDR);
    if (pd[i2] & PTE_PS) return 0;
    if (!(pd[i2] & PTE_P)) {
        pd[i2] = n | PTE_P | PTE_W | u;
        return 1;
    }
    return 0;
}

static void invlpg_if_current(aspace_t *as, virt_t page)
{
    u64 cur;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cur));
    if (cur == as->cr3_phys)
        __asm__ volatile("invlpg (%0)" :: "r"(page) : "memory");
}

/* pt_alloc then brief VMM to install. Never pmm_alloc while holding VMM. */
static status_t vmm_ensure_leaf(aspace_t *as, virt_t page)
{
    spin_lock(&as->lock);
    u64 *pte = walk_alloc(as->cr3_phys, page, false);
    spin_unlock(&as->lock);
    if (pte) return STATUS_SUCCESS;
    for (int t = 0; t < 16; t++) {
        phys_t n = pt_alloc();
        if (n == PMM_INVALID) return STATUS_NO_MEMORY;
        spin_lock(&as->lock);
        int used = walk_fill_one(as->cr3_phys, page, n);
        pte = walk_alloc(as->cr3_phys, page, false);
        spin_unlock(&as->lock);
        if (!used) pmm_free(n, 0);
        if (pte) return STATUS_SUCCESS;
    }
    return STATUS_NO_MEMORY;
}

static u64 collect_clear_locked(aspace_t *as, virt_t va, u64 n_pages,
                                phys_t *out, u32 cap, u32 *nf)
{
    u64 n = n_pages < (u64)cap ? n_pages : (u64)cap;
    for (u64 i = 0; i < n; i++) {
        virt_t page = va + i * PAGE_SIZE;
        u64 *pte = walk_alloc(as->cr3_phys, page, false);
        if (!pte) continue;
        phys_t pa = *pte & PTE_ADDR;
        *pte = 0;
        invlpg_if_current(as, page);
        if (pa && *nf < cap)
            out[(*nf)++] = pa;
    }
    return n;
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
    for (u64 i = 0; i < n_pages; i++) {
        virt_t page = va + i * PAGE_SIZE;
        status_t st = vmm_ensure_leaf(as, page);
        if (!NT_SUCCESS(st)) return st;
        spin_lock(&as->lock);
        u64 *pte = walk_alloc(as->cr3_phys, page, false);
        if (!pte) {
            spin_unlock(&as->lock);
            return STATUS_NO_MEMORY;
        }
        *pte = (pa + i * PAGE_SIZE) | flags | PTE_P;
        invlpg_if_current(as, page);
        spin_unlock(&as->lock);
    }
    return STATUS_SUCCESS;
}

status_t vmm_unmap(aspace_t *as, virt_t va, u64 n_pages)
{
    /* T15: never pmm_free while holding VMM. kalloc the frame list
       first (HEAP then VMM is legal). Chunked stack fallback if OOM. */
    if (!as || n_pages == 0) return STATUS_SUCCESS;
    phys_t stack[UNMAP_BATCH];
    phys_t *fr = stack;
    u32 cap = UNMAP_BATCH;
    int heap = 0;
    if (n_pages > UNMAP_BATCH) {
        fr = kalloc(n_pages * sizeof(phys_t));
        if (fr) {
            cap = (u32)n_pages;
            heap = 1;
        } else {
            fr = stack;
        }
    }
    u64 done = 0;
    while (done < n_pages) {
        u32 nf = 0;
        spin_lock(&as->lock);
        if (!as->cr3_phys) {
            spin_unlock(&as->lock);
            break;
        }
        u64 chunk = collect_clear_locked(as, va + done * PAGE_SIZE,
                                         n_pages - done, fr, cap, &nf);
        spin_unlock(&as->lock);
        for (u32 i = 0; i < nf; i++)
            pmm_free(fr[i], 0);
        if (chunk == 0) break;
        done += chunk;
    }
    if (heap) kfree(fr);
    return STATUS_SUCCESS;
}

/*
 * Retarget present PTEs in [base, base+size). Proto-only leaves stay
 * proto; prot lives on the VAD and populate will pick it up.
 * A NOACCESS'd frame (pa != 0, PTE_P clear) is restored when prot
 * becomes accessible again — T13 left that frame leaked.
 */
static void apply_prot_range(aspace_t *as, virt_t base, u64 size, u32 prot)
{
    for (u64 off = 0; off < size; off += PAGE_SIZE) {
        virt_t page = base + off;
        u64 *pte = walk_alloc(as->cr3_phys, page, false);
        if (!pte) continue;
        phys_t pa = *pte & PTE_ADDR;
        if (prot & PAGE_NOACCESS) {
            if (*pte & PTE_P)
                *pte = pa | PTE_SW_COMMIT | PTE_NX;
            __asm__ volatile("invlpg (%0)" :: "r"(page) : "memory");
            continue;
        }
        if ((*pte & PTE_P) || pa) {
            *pte = pa | pte_flags_from_prot(prot);
            __asm__ volatile("invlpg (%0)" :: "r"(page) : "memory");
        }
    }
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
    if (!as) return;
    spin_lock(&as->lock);
    if (!as->cr3_phys) {
        spin_unlock(&as->lock);
        return;
    }
    vad_t snap[MAX_VADS];
    u32 n = as->vad_count;
    if (n > MAX_VADS) n = MAX_VADS;
    for (u32 i = 0; i < n; i++)
        snap[i] = as->vads[i];
    as->vad_count = 0;
    as->committed_pages = 0;
    phys_t cr3 = as->cr3_phys;
    spin_unlock(&as->lock);
    for (u32 i = 0; i < n; i++) {
        u64 pages = (snap[i].end - snap[i].start) / PAGE_SIZE;
        vmm_unmap(as, snap[i].start, pages);
    }
    spin_lock(&as->lock);
    as->cr3_phys = 0;
    spin_unlock(&as->lock);
    free_user_tables(cr3, 4);
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
    if (prot & PAGE_NOACCESS) return STATUS_ACCESS_VIOLATION;
    if (write && !(prot & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)))
        return STATUS_ACCESS_VIOLATION;
#ifdef JASOS_HOST
    u32 np = (u32)((as->vads[idx].end - as->vads[idx].start) / PAGE_SIZE);
    if (np == 0) return STATUS_ACCESS_VIOLATION;
    if (!as->host_pages[idx]) {
        as->host_npages[idx] = np;
        as->host_pages[idx] = kalloc_zero(np * sizeof(u8 *));
        if (!as->host_pages[idx]) return STATUS_NO_MEMORY;
    }
    u32 pi = (u32)((page - as->vads[idx].start) / PAGE_SIZE);
    if (pi >= as->host_npages[idx]) return STATUS_ACCESS_VIOLATION;
    if (!as->host_pages[idx][pi]) {
        as->host_pages[idx][pi] = kalloc_zero(PAGE_SIZE);
        if (!as->host_pages[idx][pi]) return STATUS_NO_MEMORY;
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
    status_t st = vmm_ensure_leaf(as, page);
    if (!NT_SUCCESS(st)) {
        pmm_free(pa, 0);
        return st;
    }
    spin_lock(&as->lock);
    idx = vad_lookup(as, page);
    if (idx < 0) {
        spin_unlock(&as->lock);
        pmm_free(pa, 0);
        return STATUS_ACCESS_VIOLATION;
    }
    prot = as->vads[idx].prot;
    if (prot & PAGE_NOACCESS) {
        spin_unlock(&as->lock);
        pmm_free(pa, 0);
        return STATUS_ACCESS_VIOLATION;
    }
    if (write && !(prot & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) {
        spin_unlock(&as->lock);
        pmm_free(pa, 0);
        return STATUS_ACCESS_VIOLATION;
    }
    pte = walk_alloc(as->cr3_phys, page, false);
    if (pte && (*pte & PTE_P)) {
        spin_unlock(&as->lock);
        pmm_free(pa, 0);
        return STATUS_SUCCESS;
    }
    if (!pte) {
        spin_unlock(&as->lock);
        pmm_free(pa, 0);
        return STATUS_NO_MEMORY;
    }
    phys_t parked = *pte & PTE_ADDR;
    if (parked) {
        *pte = parked | pte_flags_from_prot(prot);
        invlpg_if_current(as, page);
        spin_unlock(&as->lock);
        pmm_free(pa, 0);
        return STATUS_SUCCESS;
    }
    *pte = pa | pte_flags_from_prot(prot);
    invlpg_if_current(as, page);
    spin_unlock(&as->lock);
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
    as->host_pages[idx] = NULL;
    as->host_npages[idx] = (u32)pages;
#endif
    as->vad_count++;
    as->committed_pages += pages;
    if (*base == 0)
        as->brk = va + size;
    spin_unlock(&as->lock);

#ifndef JASOS_HOST
    for (u64 i = 0; i < pages; i++) {
        virt_t page = va + i * PAGE_SIZE;
        status_t pst = vmm_ensure_leaf(as, page);
        if (!NT_SUCCESS(pst)) {
            vmm_free_user(p, va, size);
            return STATUS_NO_MEMORY;
        }
        spin_lock(&as->lock);
        u64 *pte = walk_alloc(as->cr3_phys, page, false);
        if (!pte) {
            spin_unlock(&as->lock);
            vmm_free_user(p, va, size);
            return STATUS_NO_MEMORY;
        }
        if (!(*pte & PTE_P) && !(*pte & PTE_ADDR))
            *pte = PTE_SW_COMMIT;
        spin_unlock(&as->lock);
    }
#endif
    *base = va;
    (void)type;
    return STATUS_SUCCESS;
}

status_t vmm_free_user(process_t *p, virt_t base, u64 size)
{
    if (!p) return STATUS_INVALID_PARAMETER;
    base = PAGE_ALIGN_DOWN(base);
    if (size)
        size = PAGE_ALIGN_UP(size);
    if (base > USER_CANONICAL_TOP)
        return STATUS_CONFLICTING_ADDRESSES;
    if (size && (base + size < base || base + size - 1 > USER_CANONICAL_TOP))
        return STATUS_CONFLICTING_ADDRESSES;

    aspace_t *as = &p->aspace;
#ifdef JASOS_HOST
    u8 **arr_left = NULL, **arr_right = NULL;
    u8 **drop_arr = NULL;
    u32 drop_lo = 0, drop_hi = 0;
#endif
#ifndef JASOS_HOST
    phys_t *fr_heap = NULL;
    phys_t fr_stack[UNMAP_BATCH];
#endif

    spin_lock(&as->lock);
    int idx;
    virt_t a, d, end;
    u32 n_left, n_mid, n_right, extra;
    u32 typ, com, old_prot;

    if (size == 0) {
        idx = -1;
        for (u32 i = 0; i < as->vad_count; i++) {
            if (as->vads[i].start == base) {
                idx = (int)i;
                break;
            }
        }
        if (idx < 0) {
            spin_unlock(&as->lock);
            return STATUS_INVALID_PARAMETER;
        }
        a = as->vads[idx].start;
        d = as->vads[idx].end;
        end = d;
        n_left = 0;
        n_mid = (u32)((d - a) / PAGE_SIZE);
        n_right = 0;
    } else {
        end = base + size;
        idx = vad_contains_range(as, base, end);
        if (idx < 0) {
            spin_unlock(&as->lock);
            return STATUS_CONFLICTING_ADDRESSES;
        }
        a = as->vads[idx].start;
        d = as->vads[idx].end;
        n_left = (u32)((base - a) / PAGE_SIZE);
        n_mid = (u32)(size / PAGE_SIZE);
        n_right = (u32)((d - end) / PAGE_SIZE);
    }
    if (n_mid == 0) {
        spin_unlock(&as->lock);
        return STATUS_INVALID_PARAMETER;
    }
    typ = as->vads[idx].type;
    com = as->vads[idx].committed;
    old_prot = as->vads[idx].prot;
    extra = (n_left && n_right) ? 1u : 0u;
    if (as->vad_count + extra > MAX_VADS) {
        spin_unlock(&as->lock);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

#ifndef JASOS_HOST
    if (n_mid > UNMAP_BATCH) {
        spin_unlock(&as->lock);
        fr_heap = kalloc(n_mid * sizeof(phys_t));
        spin_lock(&as->lock);
        if (size == 0) {
            idx = -1;
            for (u32 i = 0; i < as->vad_count; i++) {
                if (as->vads[i].start == base) { idx = (int)i; break; }
            }
        } else {
            idx = vad_contains_range(as, base, end);
        }
        if (idx < 0 || as->vads[idx].start != a || as->vads[idx].end != d) {
            spin_unlock(&as->lock);
            kfree(fr_heap);
            return STATUS_CONFLICTING_ADDRESSES;
        }
        if (as->vad_count + extra > MAX_VADS) {
            spin_unlock(&as->lock);
            kfree(fr_heap);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        typ = as->vads[idx].type;
        com = as->vads[idx].committed;
        old_prot = as->vads[idx].prot;
    }
#endif

#ifdef JASOS_HOST
    int populated = as->host_pages[idx] != NULL;
    int need_arr = populated && (n_left || n_right);
    if (need_arr) {
        spin_unlock(&as->lock);
        if (n_left) {
            arr_left = kalloc_zero(n_left * sizeof(u8 *));
            if (!arr_left) return STATUS_NO_MEMORY;
        }
        if (n_right) {
            arr_right = kalloc_zero(n_right * sizeof(u8 *));
            if (!arr_right) {
                kfree(arr_left);
                return STATUS_NO_MEMORY;
            }
        }
        spin_lock(&as->lock);
        if (idx >= (int)as->vad_count ||
            as->vads[idx].start != a || as->vads[idx].end != d) {
            spin_unlock(&as->lock);
            kfree(arr_left);
            kfree(arr_right);
            return STATUS_CONFLICTING_ADDRESSES;
        }
        if (as->vad_count + extra > MAX_VADS) {
            spin_unlock(&as->lock);
            kfree(arr_left);
            kfree(arr_right);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        if (as->host_pages[idx] && ((n_left && !arr_left) || (n_right && !arr_right))) {
            spin_unlock(&as->lock);
            kfree(arr_left);
            kfree(arr_right);
            return STATUS_NO_MEMORY;
        }
    }

    u8 **oldp = as->host_pages[idx];
    if (oldp) {
        if (n_left && arr_left) {
            for (u32 i = 0; i < n_left; i++)
                arr_left[i] = oldp[i];
        }
        if (n_right && arr_right) {
            for (u32 i = 0; i < n_right; i++)
                arr_right[i] = oldp[n_left + n_mid + i];
        }
        drop_arr = oldp;
        drop_lo = n_left;
        drop_hi = n_left + n_mid;
        as->host_pages[idx] = NULL;
        as->host_npages[idx] = 0;
    }
#else
    (void)typ;
    (void)com;
    (void)old_prot;
#endif

#ifndef JASOS_HOST
    /* Collect frames while the VAD still covers the range so a
       concurrent NtAllocate is CONFLICTING, not a hole-steal. */
    phys_t *fr = fr_heap ? fr_heap : fr_stack;
    u32 fr_cap = fr_heap ? n_mid : UNMAP_BATCH;
    u32 nf = 0;
    u32 collected = 0;
    while (collected < n_mid) {
        u32 more = 0;
        u64 chunk = collect_clear_locked(as, base + (u64)collected * PAGE_SIZE,
                                         n_mid - collected, fr + nf,
                                         fr_cap - nf, &more);
        nf += more;
        collected += (u32)chunk;
        if (chunk == 0) break;
        if (!fr_heap && collected < n_mid) {
            /* OOM fallback: drop, free this batch, relock. VAD still
               present so NtAllocate cannot steal. Populate of an
               already-cleared page leaks one frame — residual on OOM. */
            spin_unlock(&as->lock);
            for (u32 i = 0; i < nf; i++)
                pmm_free(fr[i], 0);
            nf = 0;
            spin_lock(&as->lock);
        }
    }
#endif

    if (n_left && n_right) {
        as->vads[idx].start = a;
        as->vads[idx].end = base;
        as->vads[idx].prot = old_prot;
#ifdef JASOS_HOST
        as->host_pages[idx] = arr_left ? arr_left : as->host_pages[idx];
        as->host_npages[idx] = n_left;
        arr_left = NULL;
#endif
        u32 j = vad_append(as, end, d, old_prot, typ, com);
#ifdef JASOS_HOST
        as->host_pages[j] = arr_right;
        as->host_npages[j] = n_right;
        arr_right = NULL;
#else
        (void)j;
#endif
    } else if (n_left) {
        as->vads[idx].end = base;
#ifdef JASOS_HOST
        as->host_pages[idx] = arr_left;
        as->host_npages[idx] = n_left;
        arr_left = NULL;
#endif
    } else if (n_right) {
        as->vads[idx].start = end;
#ifdef JASOS_HOST
        as->host_pages[idx] = arr_right;
        as->host_npages[idx] = n_right;
        arr_right = NULL;
#endif
    } else {
#ifdef JASOS_HOST
        as->host_pages[idx] = as->host_pages[as->vad_count - 1];
        as->host_npages[idx] = as->host_npages[as->vad_count - 1];
        as->host_pages[as->vad_count - 1] = NULL;
        as->host_npages[as->vad_count - 1] = 0;
#endif
        as->vads[idx] = as->vads[--as->vad_count];
    }

#ifndef JASOS_HOST
    /* PTEs already cleared. Rank-safe: pmm_free after VMM drop. */
#endif
    if (as->committed_pages >= n_mid)
        as->committed_pages -= n_mid;
    else
        as->committed_pages = 0;
    spin_unlock(&as->lock);

#ifndef JASOS_HOST
    for (u32 i = 0; i < nf; i++)
        pmm_free(fr[i], 0);
    kfree(fr_heap);
#endif

#ifdef JASOS_HOST
    if (drop_arr) {
        for (u32 i = drop_lo; i < drop_hi; i++) {
            if (drop_arr[i]) kfree(drop_arr[i]);
        }
        kfree(drop_arr);
    }
    kfree(arr_left);
    kfree(arr_right);
#endif
    return STATUS_SUCCESS;
}

status_t vmm_write_aspace(aspace_t *as, virt_t va, const void *src, u64 n)
{
    if (!as || !src || n == 0) return STATUS_INVALID_PARAMETER;
    const u8 *s = src;
#ifdef JASOS_HOST
    while (n) {
        virt_t page = PAGE_ALIGN_DOWN(va);
        status_t st = vmm_populate_page(as, page, false);
        if (!NT_SUCCESS(st)) return st;
        int idx = vad_lookup(as, page);
        if (idx < 0) return STATUS_ACCESS_VIOLATION;
        u32 pi = (u32)((page - as->vads[idx].start) / PAGE_SIZE);
        if (!as->host_pages[idx] || pi >= as->host_npages[idx] || !as->host_pages[idx][pi])
            return STATUS_ACCESS_VIOLATION;
        u64 off = va & PAGE_MASK;
        u64 chunk = MIN(n, PAGE_SIZE - off);
        memcpy(as->host_pages[idx][pi] + off, s, (size_t)chunk);
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
        virt_t page = PAGE_ALIGN_DOWN(va);
        status_t st = vmm_populate_page(as, page, false);
        if (!NT_SUCCESS(st)) return st;
        int idx = vad_lookup(as, page);
        if (idx < 0) return STATUS_ACCESS_VIOLATION;
        u32 pi = (u32)((page - as->vads[idx].start) / PAGE_SIZE);
        if (!as->host_pages[idx] || pi >= as->host_npages[idx] || !as->host_pages[idx][pi])
            return STATUS_ACCESS_VIOLATION;
        u64 off = va & PAGE_MASK;
        u64 chunk = MIN(n, PAGE_SIZE - off);
        memcpy(d, as->host_pages[idx][pi] + off, (size_t)chunk);
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

static int prot_legal(u32 prot)
{
    if (prot & PAGE_EXECUTE_READWRITE) return 0;
    if (prot & PAGE_NOACCESS) return 1;
    if (prot & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE | PAGE_EXECUTE_READ))
        return 1;
    return 0;
}

status_t vmm_protect_user(process_t *p, virt_t base, u64 size, u32 prot, u32 *old_prot)
{
    if (!p || size == 0) return STATUS_INVALID_PARAMETER;
    if (!prot_legal(prot)) return STATUS_INVALID_PAGE_PROTECTION;
    base = PAGE_ALIGN_DOWN(base);
    size = PAGE_ALIGN_UP(size);
    if (base > USER_CANONICAL_TOP || base + size < base ||
        (size && base + size - 1 > USER_CANONICAL_TOP))
        return STATUS_CONFLICTING_ADDRESSES;
    virt_t end = base + size;
    aspace_t *as = &p->aspace;
#ifdef JASOS_HOST
    u8 **arr_left = NULL, **arr_mid = NULL, **arr_right = NULL;
    u8 **drop_old = NULL;
#endif

    spin_lock(&as->lock);
    int idx = vad_contains_range(as, base, end);
    if (idx < 0) {
        /* T24: hole-free mixed-prot run. Snapshot clips, drop VMM,
         * then protect each clip (each is contained in one VAD).
         * Recursion bottoms out on the contained path. A hole is
         * still CONFLICTING. Partial apply if a later clip fails. */
        virt_t clo[MAX_VADS], chi[MAX_VADS];
        u32 nclip = 0;
        int first = vad_run_covers(as, base, end, &nclip);
        if (first < 0 || nclip < 2) {
            spin_unlock(&as->lock);
            return STATUS_CONFLICTING_ADDRESSES;
        }
        u32 old_run = as->vads[first].prot;
        nclip = 0;
        virt_t cur = base;
        while (cur < end && nclip < MAX_VADS) {
            int i = vad_lookup(as, cur);
            if (i < 0) {
                spin_unlock(&as->lock);
                return STATUS_CONFLICTING_ADDRESSES;
            }
            virt_t hi = as->vads[i].end;
            if (hi > end) hi = end;
            if (hi <= cur) {
                spin_unlock(&as->lock);
                return STATUS_CONFLICTING_ADDRESSES;
            }
            clo[nclip] = cur;
            chi[nclip] = hi;
            nclip++;
            cur = hi;
        }
        spin_unlock(&as->lock);
        if (cur < end) return STATUS_INSUFFICIENT_RESOURCES;
        if (old_prot) *old_prot = old_run;
        for (u32 k = 0; k < nclip; k++) {
            u32 ign = 0;
            status_t st = vmm_protect_user(p, clo[k], chi[k] - clo[k], prot, &ign);
            if (!NT_SUCCESS(st)) return st;
        }
        return STATUS_SUCCESS;
    }
    virt_t a = as->vads[idx].start;
    virt_t d = as->vads[idx].end;
    u32 old = as->vads[idx].prot;
    u32 typ = as->vads[idx].type;
    u32 com = as->vads[idx].committed;
    u32 n_left = (u32)((base - a) / PAGE_SIZE);
    u32 n_mid = (u32)(size / PAGE_SIZE);
    u32 n_right = (u32)((d - end) / PAGE_SIZE);
    u32 extra = (n_left ? 1u : 0u) + (n_right ? 1u : 0u);
    if (as->vad_count + extra > MAX_VADS) {
        spin_unlock(&as->lock);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (extra == 0) {
        if (old_prot) *old_prot = old;
        as->vads[idx].prot = prot;
#ifndef JASOS_HOST
        apply_prot_range(as, base, size, prot);
#endif
        spin_unlock(&as->lock);
        vad_coalesce(as);
        return STATUS_SUCCESS;
    }

#ifdef JASOS_HOST
    int populated = as->host_pages[idx] != NULL;
    spin_unlock(&as->lock);
    if (populated) {
        if (n_left) {
            arr_left = kalloc_zero(n_left * sizeof(u8 *));
            if (!arr_left) return STATUS_NO_MEMORY;
        }
        arr_mid = kalloc_zero(n_mid * sizeof(u8 *));
        if (!arr_mid) {
            kfree(arr_left);
            return STATUS_NO_MEMORY;
        }
        if (n_right) {
            arr_right = kalloc_zero(n_right * sizeof(u8 *));
            if (!arr_right) {
                kfree(arr_left);
                kfree(arr_mid);
                return STATUS_NO_MEMORY;
            }
        }
    }
    spin_lock(&as->lock);
    if (idx >= (int)as->vad_count ||
        as->vads[idx].start != a || as->vads[idx].end != d) {
        spin_unlock(&as->lock);
        kfree(arr_left);
        kfree(arr_mid);
        kfree(arr_right);
        return STATUS_CONFLICTING_ADDRESSES;
    }
    if (as->vad_count + extra > MAX_VADS) {
        spin_unlock(&as->lock);
        kfree(arr_left);
        kfree(arr_mid);
        kfree(arr_right);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (as->host_pages[idx] && (!arr_mid || (n_left && !arr_left) || (n_right && !arr_right))) {
        spin_unlock(&as->lock);
        kfree(arr_left);
        kfree(arr_mid);
        kfree(arr_right);
        return STATUS_NO_MEMORY;
    }
    drop_old = as->host_pages[idx];
    if (drop_old) {
        for (u32 i = 0; i < n_left; i++)
            arr_left[i] = drop_old[i];
        for (u32 i = 0; i < n_mid; i++)
            arr_mid[i] = drop_old[n_left + i];
        for (u32 i = 0; i < n_right; i++)
            arr_right[i] = drop_old[n_left + n_mid + i];
    }
#else
    /* hardware: still holding as->lock from the first acquire. */
#endif

    if (old_prot) *old_prot = old;
    as->vads[idx].start = base;
    as->vads[idx].end = end;
    as->vads[idx].prot = prot;
#ifdef JASOS_HOST
    as->host_pages[idx] = arr_mid;
    as->host_npages[idx] = n_mid;
    arr_mid = NULL;
#endif
    if (n_left) {
        u32 j = vad_append(as, a, base, old, typ, com);
#ifdef JASOS_HOST
        as->host_pages[j] = arr_left;
        as->host_npages[j] = n_left;
        arr_left = NULL;
#else
        (void)j;
#endif
    }
    if (n_right) {
        u32 j = vad_append(as, end, d, old, typ, com);
#ifdef JASOS_HOST
        as->host_pages[j] = arr_right;
        as->host_npages[j] = n_right;
        arr_right = NULL;
#else
        (void)j;
#endif
    }
#ifndef JASOS_HOST
    apply_prot_range(as, base, size, prot);
#endif
    spin_unlock(&as->lock);
#ifdef JASOS_HOST
    kfree(drop_old);
    kfree(arr_left);
    kfree(arr_mid);
    kfree(arr_right);
#endif
    vad_coalesce(as);
    return STATUS_SUCCESS;
}

