#include <jasos/ke.h>
#include <jasos/kprintf.h>
#include <jasos/status.h>

/*
 * Wait/signal. Sleeps. Not a spinlock in a trench coat.
 *
 * Why this will fail in production:
 *  - Multi-object wait (WaitForMultipleObjects) is not implemented.
 *  - A thread killed while on a wait list is not purged in v1; we
 *    require exit to run sched_exit_thread which does not walk every
 *    object. Residual: dying-while-waiting is a leak of the wait block
 *    (the thread object stays referenced by the list until signal).
 * Fixed here: mutex ownership transfers in disp_wake_one; abandoned
 * mutex wakes with STATUS_ABANDONED; wait never holds DISP across
 * a SCHED acquire without the T3 ranking (DISP=9 < SCHED=10).
 */

status_t ke_wait_object(dispatcher_t *d, u64 timeout_ticks)
{
    if (!d) return STATUS_INVALID_PARAMETER;
    thread_t *t = ke_current();
    if (!t) return STATUS_INVALID_PARAMETER;

    spin_lock(&d->lock);
    if (d->type == DISP_MUTANT) {
        mutex_object_t *m = CONTAINER_OF(d, mutex_object_t, disp);
        if (m->owner == t) {
            m->recursion++;
            spin_unlock(&d->lock);
            return STATUS_SUCCESS;
        }
        if (d->signal_state > 0) {
            d->signal_state--;
            m->owner = t;
            m->recursion = 1;
            status_t st = m->abandoned ? STATUS_ABANDONED : STATUS_SUCCESS;
            m->abandoned = false;
            spin_unlock(&d->lock);
            return st;
        }
    } else if (d->signal_state > 0) {
        if (d->type == DISP_SYNCHRONIZATION_EVENT || d->type == DISP_TIMER)
            d->signal_state = 0;
        spin_unlock(&d->lock);
        return STATUS_SUCCESS;
    }

    if (timeout_ticks == 0) {
        spin_unlock(&d->lock);
        return STATUS_TIMEOUT;
    }

    t->wait.thread = t;
    t->wait.object = d;
    t->wait.wake_status = STATUS_TIMEOUT;
    list_init(&t->wait.obj_link);
    list_init(&t->wait.thr_link);
    list_insert_tail(&d->wait_list, &t->wait.obj_link);
    t->state = THR_WAITING;
    t->wait_timed = (timeout_ticks != (u64)-1);
    t->wait_timeout_tick = ke_ticks() + (timeout_ticks == (u64)-1 ? 0 : timeout_ticks);
    spin_unlock(&d->lock);
    sched_reschedule();
    return t->wait.wake_status;
}

status_t ke_set_event(event_object_t *e)
{
    if (!e) return STATUS_INVALID_PARAMETER;
    disp_signal(&e->disp, 1);
    return STATUS_SUCCESS;
}

status_t ke_reset_event(event_object_t *e)
{
    if (!e) return STATUS_INVALID_PARAMETER;
    spin_lock(&e->disp.lock);
    e->disp.signal_state = 0;
    spin_unlock(&e->disp.lock);
    return STATUS_SUCCESS;
}

status_t ke_release_mutex(mutex_object_t *m)
{
    if (!m) return STATUS_INVALID_PARAMETER;
    thread_t *t = ke_current();
    spin_lock(&m->disp.lock);
    if (!t || m->owner != t || m->recursion == 0) {
        spin_unlock(&m->disp.lock);
        return STATUS_MUTANT_NOT_OWNED;
    }
    if (--m->recursion > 0) {
        spin_unlock(&m->disp.lock);
        return STATUS_SUCCESS;
    }
    m->owner = NULL;
    if (list_empty(&m->disp.wait_list)) {
        m->disp.signal_state = 1;
        spin_unlock(&m->disp.lock);
    } else {
        /* Transfer while still holding DISP; wake_one takes SCHED. */
        disp_wake_one(&m->disp, STATUS_SUCCESS);
        spin_unlock(&m->disp.lock);
    }
    if (t->priority != t->saved_priority) {
        t->priority = t->saved_priority;
        ke_pcb()->need_resched = true;
    }
    return STATUS_SUCCESS;
}

status_t ke_acquire_mutex(mutex_object_t *m, u64 timeout_ticks)
{
    return ke_wait_object(&m->disp, timeout_ticks);
}
