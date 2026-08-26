#include <jasos/mm.h>
#include <jasos/kprintf.h>
#include <jasos/string.h>
#include <jasos/ke.h>

/*
 * Buddy PMM.
 *
 * Why this will fail in production:
 *  - Metadata is a linear frame_t array; 1 TiB RAM would be 2 GiB of metadata
 *    at 8 bytes/page. Fine for 256 MiB..32 GiB, not for a supercomputer.
 *  - Coalesce walks parent order; a smashed next_pfn corrupts the free list
 *    silently until the next alloc panics.
 *  - No page coloring, no NUMA.
 * Fixed here: every alloc/free validates pfn range and order; free of a
 * non-head page panics instead of corrupting the list.
 */

#define FRAME_FREE     0
#define FRAME_TAIL     1
#define FRAME_KERNEL   2
#define FRAME_USER     3
#define FRAME_RESERVED 4

#define NIL 0xFFFFFFFFu

static frame_t     *g_frames;
static u64          g_nframes;
static phys_t       g_phys_base;
static u32          g_free_head[PMM_ORDERS];
static u64          g_free_pages;
static u64          g_total_avail;
static spinlock_t   g_pmm_lock = SPINLOCK_INIT("pmm", LOCK_RANK_PMM);
#ifdef JASOS_HOST
static u8          *g_host_ram;
#endif
#ifndef JASOS_HOST
static bool         g_hhdm_live;
static phys_t       g_frames_phys;
#endif

#ifdef JASOS_HOST
u8 *pmm_host_ram(void) { return g_host_ram; }
phys_t pmm_host_base(void) { return g_phys_base; }
#endif

static u32 pfn_of(phys_t p)
{
    return (u32)((p - g_phys_base) >> PAGE_SHIFT);
}

static phys_t phys_of(u32 pfn)
{
    return g_phys_base + ((phys_t)pfn << PAGE_SHIFT);
}

static bool pfn_ok(u32 pfn)
{
    return pfn < g_nframes;
}

static void list_push(u32 order, u32 pfn)
{
    g_frames[pfn].next_pfn = g_free_head[order];
    g_free_head[order] = pfn;
}

static u32 list_pop(u32 order)
{
    u32 pfn = g_free_head[order];
    if (pfn == NIL) return NIL;
    g_free_head[order] = g_frames[pfn].next_pfn;
    g_frames[pfn].next_pfn = NIL;
    return pfn;
}

static void pmm_list_remove(u32 order, u32 pfn)
{
    u32 *w = &g_free_head[order];
    while (*w != NIL) {
        if (*w == pfn) {
            *w = g_frames[pfn].next_pfn;
            g_frames[pfn].next_pfn = NIL;
            return;
        }
        w = &g_frames[*w].next_pfn;
    }
}

static void mark_block(u32 pfn, u32 order, u8 state)
{
    u32 n = 1u << order;
    g_frames[pfn].order = (u8)order;
    g_frames[pfn].state = state;
    for (u32 i = 1; i < n && pfn + i < g_nframes; i++) {
        g_frames[pfn + i].order = (u8)order;
        g_frames[pfn + i].state = FRAME_TAIL;
    }
}

void pmm_init(const mmap_entry_t *map, u32 count, phys_t kphys, u64 ksize)
{
    phys_t max = 0;
    phys_t min = ~0ULL;

    for (u32 i = 0; i < count; i++) {
        if (map[i].type != 1) continue;
        phys_t end = map[i].base + map[i].length;
        if (map[i].base < min) min = map[i].base;
        if (end > max) max = end;
    }
    if (min == ~0ULL || max <= min) panic("pmm: no available ram");

    min = PAGE_ALIGN_DOWN(min);
    max = PAGE_ALIGN_DOWN(max);
    g_phys_base = min;
    g_nframes   = (max - min) >> PAGE_SHIFT;
    if (g_nframes < 4096) panic("pmm: ram");

    usize meta = g_nframes * sizeof(frame_t);
    meta = PAGE_ALIGN_UP(meta);

#ifdef JASOS_HOST
    g_host_ram = calloc(1, (size_t)(g_nframes * PAGE_SIZE + meta + PAGE_SIZE));
    if (!g_host_ram) panic("pmm: host calloc");
    g_frames = (frame_t *)g_host_ram;
    /* Shift phys_base conceptually; host addrs are not the pfn math.
       We keep pfn 0 at the start of the RAM pool AFTER metadata. */
    u8 *pool = (u8 *)PAGE_ALIGN_UP((u64)(g_host_ram + meta));
    g_phys_base = (phys_t)(uintptr_t)pool;
    g_frames = (frame_t *)g_host_ram;
    (void)kphys;
    (void)ksize;
#else
    /* Steal metadata from the first available region above 1 MiB that fits. */
    phys_t meta_phys = 0;
    for (u32 i = 0; i < count; i++) {
        if (map[i].type != 1) continue;
        phys_t b = PAGE_ALIGN_UP(map[i].base);
        if (b < 0x100000) b = 0x100000;
        if (b + meta <= map[i].base + map[i].length) {
            meta_phys = b;
            break;
        }
    }
    if (!meta_phys) panic("pmm: no room for metadata");
    g_frames_phys = meta_phys;
    /* Identity still live during pmm_init. vmm_init calls pmm_enter_hhdm. */
    g_frames = (frame_t *)(uintptr_t)meta_phys;
#endif

    memset(g_frames, 0, (size_t)(g_nframes * sizeof(frame_t)));
    for (u32 i = 0; i < PMM_ORDERS; i++) g_free_head[i] = NIL;
    for (u64 i = 0; i < g_nframes; i++) {
        g_frames[i].state = FRAME_RESERVED;
        g_frames[i].next_pfn = NIL;
    }

#ifdef JASOS_HOST
    /* Host RAM is the calloc pool; every tracked frame is available. */
    for (u64 i = 0; i < g_nframes; i++) g_frames[i].state = FRAME_FREE;
#else
    for (u32 i = 0; i < count; i++) {
        if (map[i].type != 1) continue;
        phys_t b = PAGE_ALIGN_UP(map[i].base);
        phys_t e = PAGE_ALIGN_DOWN(map[i].base + map[i].length);
        if (b < 0x100000) b = 0x100000;
        if (e <= b) continue;
        u32 pf = pfn_of(b);
        u32 pe = pfn_of(e);
        if (pf >= g_nframes) continue;
        if (pe > g_nframes) pe = (u32)g_nframes;
        for (u32 p = pf; p < pe; p++) g_frames[p].state = FRAME_FREE;
    }
    /* Reserve kernel image. */
    phys_t kb = PAGE_ALIGN_DOWN(kphys);
    phys_t ke = PAGE_ALIGN_UP(kphys + ksize);
    u32 kpf = pfn_of(kb), kpe = pfn_of(ke);
    if (kpe > g_nframes) kpe = (u32)g_nframes;
    for (u32 p = kpf; p < kpe && pfn_ok(p); p++) g_frames[p].state = FRAME_RESERVED;
    /* Reserve metadata pages. */
    {
        u32 mpf = pfn_of((phys_t)(uintptr_t)g_frames);
        u32 mpe = mpf + (u32)(meta >> PAGE_SHIFT);
        for (u32 p = mpf; p < mpe && pfn_ok(p); p++) g_frames[p].state = FRAME_RESERVED;
    }
#endif
    /* Build buddy lists from free runs, largest order first. */
    g_free_pages = 0;
    g_total_avail = 0;
    u32 p = 0;
    while (p < g_nframes) {
        if (g_frames[p].state != FRAME_FREE) { p++; continue; }
        u32 order = 0;
        while (order < PMM_MAX_ORDER) {
            u32 n = 1u << (order + 1);
            if (p % n != 0) break;
            if (p + n > g_nframes) break;
            bool ok = true;
            for (u32 j = 0; j < n; j++) {
                if (g_frames[p + j].state != FRAME_FREE) { ok = false; break; }
            }
            if (!ok) break;
            order++;
        }
        mark_block(p, order, FRAME_FREE);
        list_push(order, p);
        g_free_pages += (1ull << order);
        g_total_avail += (1ull << order);
        p += (1u << order);
    }

    kprintf("pmm: %llu pages (%llu MiB) free, %llu frames tracked\n",
            (unsigned long long)g_free_pages,
            (unsigned long long)(g_free_pages * PAGE_SIZE / (1024 * 1024)),
            (unsigned long long)g_nframes);
}

phys_t pmm_alloc(u32 order, u32 flags)
{
    if (order > PMM_MAX_ORDER) return PMM_INVALID;
    spin_lock(&g_pmm_lock);
    u32 o = order;
    while (o <= PMM_MAX_ORDER && g_free_head[o] == NIL) o++;
    if (o > PMM_MAX_ORDER) {
        spin_unlock(&g_pmm_lock);
        return PMM_INVALID;
    }
    u32 pfn = list_pop(o);
    while (o > order) {
        o--;
        u32 buddy = pfn + (1u << o);
        mark_block(buddy, o, FRAME_FREE);
        list_push(o, buddy);
        mark_block(pfn, o, FRAME_FREE);
    }
    u8 st = (flags & PMM_USER) ? FRAME_USER : FRAME_KERNEL;
    mark_block(pfn, order, st);
    g_free_pages -= (1ull << order);
    phys_t pa = phys_of(pfn);
    spin_unlock(&g_pmm_lock);

    if (flags & PMM_ZERO) {
        memset(pmm_phys_to_virt(pa), 0, (size_t)PAGE_SIZE << order);
    }
    return pa;
}

void pmm_free(phys_t p, u32 order)
{
    if (p == PMM_INVALID || order > PMM_MAX_ORDER) panic("pmm: bad free");
    u32 pfn = pfn_of(p);
    spin_lock(&g_pmm_lock);
    if (!pfn_ok(pfn) || g_frames[pfn].state == FRAME_FREE ||
        g_frames[pfn].state == FRAME_TAIL || g_frames[pfn].state == FRAME_RESERVED) {
        spin_unlock(&g_pmm_lock);
        panic("pmm: free of non-head pfn %u state %u", pfn, g_frames[pfn].state);
    }
    mark_block(pfn, order, FRAME_FREE);
    g_free_pages += (1ull << order);

    while (order < PMM_MAX_ORDER) {
        u32 n = 1u << order;
        u32 buddy = pfn ^ n;
        if (!pfn_ok(buddy)) break;
        if (g_frames[buddy].state != FRAME_FREE || g_frames[buddy].order != order) break;
        pmm_list_remove(order, buddy);
        if (buddy < pfn) pfn = buddy;
        order++;
        mark_block(pfn, order, FRAME_FREE);
    }
    list_push(order, pfn);
    spin_unlock(&g_pmm_lock);
}

u64 pmm_free_pages(void) { return g_free_pages; }
u64 pmm_total_pages(void) { return g_total_avail; }

void *pmm_phys_to_virt(phys_t pa)
{
#ifdef JASOS_HOST
    return (void *)(uintptr_t)pa;
#else
    if (g_hhdm_live) return (void *)(uintptr_t)(HHDM_BASE + pa);
    return (void *)(uintptr_t)pa;
#endif
}

void pmm_enter_hhdm(void)
{
#ifndef JASOS_HOST
    g_hhdm_live = true;
    g_frames = (frame_t *)(uintptr_t)(HHDM_BASE + g_frames_phys);
#endif
}

void pmm_dump(void)
{
    kprintf("pmm free-list heads:");
    for (u32 o = 0; o < PMM_ORDERS; o++) {
        u32 n = 0, p = g_free_head[o];
        while (p != NIL && n < 10000) { n++; p = g_frames[p].next_pfn; }
        if (n) kprintf(" o%u=%u", o, n);
    }
    kprintf("\n");
}
