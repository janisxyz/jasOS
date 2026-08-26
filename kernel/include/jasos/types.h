#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>

#ifdef JASOS_HOST
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#endif

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;
typedef size_t   usize;

typedef u32 status_t;
typedef u64 handle_t;
typedef u64 phys_t;
typedef u64 virt_t;
typedef u32 access_t;
typedef u64 kpid_t;
typedef u64 ktid_t;

#define PACKED      __attribute__((packed))
#define ALIGNED(n)  __attribute__((aligned(n)))
#define NORETURN    __attribute__((noreturn))
#define UNUSED      __attribute__((unused))
#define ALWAYS_INLINE static inline __attribute__((always_inline))

#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b)   ((a) < (b) ? (a) : (b))
#define MAX(a, b)   ((a) > (b) ? (a) : (b))

#define OFFSET_OF(t, m) ((u64)&((t *)0)->m)
#define CONTAINER_OF(p, t, m) ((t *)((u8 *)(p) - OFFSET_OF(t, m)))

ALWAYS_INLINE void cpu_relax(void)
{
#ifdef JASOS_HOST
    __asm__ volatile("" ::: "memory");
#else
    __asm__ volatile("pause" ::: "memory");
#endif
}

ALWAYS_INLINE void memory_barrier(void)
{
    __asm__ volatile("" ::: "memory");
}

ALWAYS_INLINE u64 atomic_inc64(volatile u64 *p)
{
    return __sync_add_and_fetch(p, 1);
}

ALWAYS_INLINE u64 atomic_dec64(volatile u64 *p)
{
    return __sync_sub_and_fetch(p, 1);
}

ALWAYS_INLINE u32 atomic_inc32(volatile u32 *p)
{
    return __sync_add_and_fetch(p, 1);
}

ALWAYS_INLINE u32 atomic_dec32(volatile u32 *p)
{
    return __sync_sub_and_fetch(p, 1);
}

typedef struct spinlock {
    volatile u32 locked;
    u32          rank;
    const char  *name;
    u32          owner_cpu;
} spinlock_t;

#define SPINLOCK_INIT(n, r) { .locked = 0, .rank = (r), .name = (n), .owner_cpu = 0 }

ALWAYS_INLINE void spin_init(spinlock_t *l, const char *name, u32 rank)
{
    l->locked    = 0;
    l->rank      = rank;
    l->name      = name;
    l->owner_cpu = 0;
}

void spin_lock(spinlock_t *l);
void spin_unlock(spinlock_t *l);
bool spin_try(spinlock_t *l);

typedef struct list_entry {
    struct list_entry *next;
    struct list_entry *prev;
} list_t;

ALWAYS_INLINE void list_init(list_t *h)
{
    h->next = h;
    h->prev = h;
}

ALWAYS_INLINE bool list_empty(const list_t *h)
{
    return h->next == h;
}

ALWAYS_INLINE void list_insert_tail(list_t *h, list_t *e)
{
    e->next       = h;
    e->prev       = h->prev;
    h->prev->next = e;
    h->prev       = e;
}

ALWAYS_INLINE void list_insert_head(list_t *h, list_t *e)
{
    e->next       = h->next;
    e->prev       = h;
    h->next->prev = e;
    h->next       = e;
}

ALWAYS_INLINE void list_remove(list_t *e)
{
    e->next->prev = e->prev;
    e->prev->next = e->next;
    e->next = e;
    e->prev = e;
}

#define LIST_FOR_EACH_SAFE(pos, n, h) \
    for (pos = (h)->next, n = pos->next; pos != (h); pos = n, n = pos->next)
