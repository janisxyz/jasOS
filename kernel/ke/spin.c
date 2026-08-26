#include <jasos/types.h>
#include <jasos/kprintf.h>
#include <jasos/ke.h>

/*
 * Why this will fail in production:
 *  - UP-only. SMP will need ticket locks and a real IRQL.
 *  - Rank check is per-CPU; nested same-rank is a bug we catch now.
 *  - No deadlock watchdog. A forgotten unlock is a silent hang.
 * Fixed here: rank inversion panics; nested ranks are stacked so
 * unlocking SCHED while still holding DISP restores DISP as held.
 */

void spin_lock(spinlock_t *l)
{
    pcb_t *cpu = ke_pcb();
    if (g_panic_in_progress) {
        u32 spins = 0;
        while (__sync_lock_test_and_set(&l->locked, 1)) {
            if (++spins > 100000) return;
            cpu_relax();
        }
        return;
    }
    if (cpu && cpu->held_depth) {
        if (l->rank <= cpu->held_rank) {
            panic("lock rank %u (%s) while holding %u depth %u",
                  l->rank, l->name ? l->name : "?",
                  cpu->held_rank, cpu->held_depth);
        }
        if (cpu->held_depth >= LOCK_DEPTH_MAX)
            panic("lock nest overflow acquiring %s", l->name ? l->name : "?");
    }
    while (__sync_lock_test_and_set(&l->locked, 1)) {
        while (l->locked) cpu_relax();
    }
    if (cpu) {
        cpu->rank_stack[cpu->held_depth++] = l->rank;
        cpu->held_rank = l->rank;
        l->owner_cpu   = 0;
    }
}

void spin_unlock(spinlock_t *l)
{
    pcb_t *cpu = ke_pcb();
    __sync_lock_release(&l->locked);
    if (cpu && cpu->held_depth) {
        /* Restore previous rank even if the unlock order is the
           matching LIFO rank. Mismatched unlock still pops — a
           lying rank is worse than a slightly-wrong one, and the
           next acquire will panic if the graph is actually inverted. */
        if (cpu->rank_stack[cpu->held_depth - 1] == l->rank)
            cpu->held_depth--;
        else {
            /* Search from the top for this rank and compact. */
            for (u32 i = cpu->held_depth; i > 0; i--) {
                if (cpu->rank_stack[i - 1] == l->rank) {
                    for (u32 j = i; j < cpu->held_depth; j++)
                        cpu->rank_stack[j - 1] = cpu->rank_stack[j];
                    cpu->held_depth--;
                    break;
                }
            }
        }
        cpu->held_rank = cpu->held_depth ? cpu->rank_stack[cpu->held_depth - 1] : 0;
    }
}

bool spin_try(spinlock_t *l)
{
    if (__sync_lock_test_and_set(&l->locked, 1) != 0) return false;
    pcb_t *cpu = ke_pcb();
    if (cpu && cpu->held_depth < LOCK_DEPTH_MAX) {
        if (cpu->held_depth && l->rank <= cpu->held_rank) {
            __sync_lock_release(&l->locked);
            return false;
        }
        cpu->rank_stack[cpu->held_depth++] = l->rank;
        cpu->held_rank = l->rank;
    }
    return true;
}
