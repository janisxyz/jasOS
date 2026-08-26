#include <jasos/mm.h>
#include <jasos/kprintf.h>
#include <jasos/string.h>
#include <jasos/ke.h>

/*
 * Slab heap.
 *
 * Why this will fail in production:
 *  - kfree is O(n_slabs) pointer classify. Fine until we have thousands.
 *  - No per-CPU magazines, no magazine reaping.
 *  - Large allocs never split on free (buddy takes the whole order).
 * Fixed here: freelist bitmap catches double-free; poison 0xDB/0xAA;
 * foreign pointer panics instead of corrupting a slab.
 */

#define SLAB_MAGIC 0x514C4142u /* 'QLAB' */
#define POISON_FREE 0xDB
#define POISON_UNINIT 0xAA

static const u32 k_sizes[HEAP_CLASSES] = { 16, 32, 64, 128, 256, 512, 1024, 2048, 4096 };

typedef struct slab {
    u32           magic;
    u32           size;
    u32           objects;
    u32           free_count;
    u8           *base;
    u8           *bitmap; /* 1 = free */
    struct slab  *next;
    phys_t        phys;
    u32           pages;
    bool          large;
} slab_t;

static slab_t    *g_classes[HEAP_CLASSES];
static slab_t    *g_large;
static spinlock_t g_heap_lock = SPINLOCK_INIT("heap", LOCK_RANK_HEAP);
static u64        g_used;
static bool       g_heap_up;

static int class_of(usize n)
{
    for (int i = 0; i < (int)HEAP_CLASSES; i++) {
        if (n <= k_sizes[i]) return i;
    }
    return -1;
}

static void bitmap_set(u8 *bm, u32 i, bool free)
{
    if (free) bm[i / 8] |= (u8)(1u << (i % 8));
    else      bm[i / 8] &= (u8)~(1u << (i % 8));
}

static bool bitmap_get(u8 *bm, u32 i)
{
    return (bm[i / 8] >> (i % 8)) & 1u;
}

static slab_t *slab_new(u32 size)
{
    u32 pages = 1;
    if (size >= 1024) pages = 2;
    /* pmm is rank 2, heap is rank 3 — drop heap before touching pmm. */
    spin_unlock(&g_heap_lock);
    phys_t pa = pmm_alloc(pages == 1 ? 0 : 1, PMM_KERNEL | PMM_ZERO);
    spin_lock(&g_heap_lock);
    if (pa == PMM_INVALID) return NULL;
#ifdef JASOS_HOST
    u8 *base = (u8 *)(uintptr_t)pa;
#else
    u8 *base = (u8 *)(uintptr_t)(HHDM_BASE + pa);
#endif
    slab_t *s = (slab_t *)base;
    s->magic = SLAB_MAGIC;
    s->size = size;
    s->pages = pages;
    s->phys = pa;
    s->large = false;
    u32 header = (u32)sizeof(slab_t) + 64;
    s->base = base + header;
    s->objects = (u32)((pages * PAGE_SIZE - header) / size);
    if (s->objects == 0) {
        spin_unlock(&g_heap_lock);
        pmm_free(pa, pages == 1 ? 0 : 1);
        spin_lock(&g_heap_lock);
        return NULL;
    }
    s->bitmap = s->base - ((s->objects + 7) / 8);
    /* If bitmap collides with header, shrink objects. */
    while (s->bitmap < (u8 *)(s + 1) && s->objects > 1) {
        s->objects--;
        s->bitmap = s->base - ((s->objects + 7) / 8);
    }
    memset(s->bitmap, 0xFF, (s->objects + 7) / 8);
    s->free_count = s->objects;
    s->next = NULL;
    return s;
}

void heap_init(void)
{
    memset(g_classes, 0, sizeof(g_classes));
    g_large = NULL;
    g_used = 0;
    g_heap_up = true;
    kprintf("heap: slab classes 16..4096 ready\n");
}

static void *slab_alloc(slab_t **head, u32 size)
{
    slab_t *s = *head;
    while (s && s->free_count == 0) s = s->next;
    if (!s) {
        s = slab_new(size);
        if (!s) return NULL;
        s->next = *head;
        *head = s;
    }
    for (u32 i = 0; i < s->objects; i++) {
        if (bitmap_get(s->bitmap, i)) {
            bitmap_set(s->bitmap, i, false);
            s->free_count--;
            u8 *p = s->base + (usize)i * s->size;
            memset(p, POISON_UNINIT, s->size);
            g_used += s->size;
            return p;
        }
    }
    return NULL;
}

void *kalloc(usize n)
{
    if (!g_heap_up || n == 0) return NULL;
    if (n > 8 * 1024 * 1024) return NULL;
    spin_lock(&g_heap_lock);
    int c = class_of(n);
    void *p;
    if (c >= 0) {
        p = slab_alloc(&g_classes[c], k_sizes[c]);
    } else {
        u32 pages = (u32)((PAGE_ALIGN_UP(n + sizeof(slab_t))) / PAGE_SIZE);
        u32 order = 0;
        while ((1u << order) < pages) order++;
        spin_unlock(&g_heap_lock);
        phys_t pa = pmm_alloc(order, PMM_KERNEL | PMM_ZERO);
        spin_lock(&g_heap_lock);
        if (pa == PMM_INVALID) {
            spin_unlock(&g_heap_lock);
            return NULL;
        }
#ifdef JASOS_HOST
        u8 *base = (u8 *)(uintptr_t)pa;
#else
        u8 *base = (u8 *)(uintptr_t)(HHDM_BASE + pa);
#endif
        slab_t *s = (slab_t *)base;
        s->magic = SLAB_MAGIC;
        s->size = (u32)n;
        s->pages = 1u << order;
        s->phys = pa;
        s->large = true;
        s->objects = 1;
        s->free_count = 0;
        s->base = base + sizeof(slab_t);
        s->bitmap = NULL;
        s->next = g_large;
        g_large = s;
        g_used += n;
        p = s->base;
    }
    spin_unlock(&g_heap_lock);
    return p;
}

void *kalloc_zero(usize n)
{
    void *p = kalloc(n);
    if (p) memset(p, 0, n);
    return p;
}

static bool in_slab(slab_t *s, void *p, u32 *idx)
{
    if (!s || s->magic != SLAB_MAGIC) return false;
    u8 *q = p;
    if (q < s->base) return false;
    usize off = (usize)(q - s->base);
    if (s->large) {
        if (idx) *idx = 0;
        return q == s->base;
    }
    if (off % s->size) return false;
    u32 i = (u32)(off / s->size);
    if (i >= s->objects) return false;
    if (idx) *idx = i;
    return true;
}

void kfree(void *p)
{
    if (!p) return;
    spin_lock(&g_heap_lock);
    for (int c = 0; c < (int)HEAP_CLASSES; c++) {
        for (slab_t *s = g_classes[c]; s; s = s->next) {
            u32 i;
            if (!in_slab(s, p, &i)) continue;
            if (bitmap_get(s->bitmap, i)) {
                spin_unlock(&g_heap_lock);
                panic("heap df %p class %d", p, c);
            }
            bitmap_set(s->bitmap, i, true);
            s->free_count++;
            memset(p, POISON_FREE, s->size);
            g_used -= s->size;
            spin_unlock(&g_heap_lock);
            return;
        }
    }
    slab_t **w = &g_large;
    while (*w) {
        slab_t *s = *w;
        u32 i;
        if (in_slab(s, p, &i)) {
            *w = s->next;
            g_used -= s->size;
            u32 order = 0;
            while ((1u << order) < s->pages) order++;
            s->magic = 0;
            phys_t pa = s->phys;
            spin_unlock(&g_heap_lock);
            pmm_free(pa, order);
            return;
        }
        w = &s->next;
    }
    spin_unlock(&g_heap_lock);
    panic("heap foreign %p", p);
}

u64 heap_used(void) { return g_used; }
