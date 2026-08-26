#include <jasos/types.h>
#include <jasos/kprintf.h>
#include <jasos/ke.h>

/*
 * Why this will fail in production:
 *  - UP-only. SMP will need ticket locks and a real IRQL.
 *  - Rank check is best-effort (per-CPU held_rank); nested same-rank is a bug
 *    we do not catch.
 *  - No deadlock watchdog. A forgotten unlock is a silent hang.
 * Fixed here: rank inversion panics instead of hanging the box later.
 */

void spin_lock(spinlock_t *l)
{
    pcb_t *cpu = ke_pcb();
    if (g_panic_in_progress) {
        /* Panic path: try, but do not spin forever. */
        u32 spins = 0;
        while (__sync_lock_test_and_set(&l->locked, 1)) {
            if (++spins > 100000) return;
            cpu_relax();
        }
        return;
    }
    if (cpu && cpu->held_rank && l->rank <= cpu->held_rank) {
        panic("lock rank %u (%s) while holding %u", l->rank,
              l->name ? l->name : "?", cpu->held_rank);
    }
    while (__sync_lock_test_and_set(&l->locked, 1)) {
        while (l->locked) cpu_relax();
    }
    if (cpu) {
        cpu->held_rank = l->rank;
        l->owner_cpu   = 0;
    }
}

void spin_unlock(spinlock_t *l)
{
    pcb_t *cpu = ke_pcb();
    __sync_lock_release(&l->locked);
    if (cpu && cpu->held_rank == l->rank) cpu->held_rank = 0;
}

bool spin_try(spinlock_t *l)
{
    return __sync_lock_test_and_set(&l->locked, 1) == 0;
}
