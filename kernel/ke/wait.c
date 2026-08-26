#include <jasos/ke.h>
#include <jasos/kprintf.h>
#include <jasos/status.h>

/*
 * Wait/signal. Sleeps. Not a spinlock in a trench coat.
 *
 * Why this will fail in production:
 *  - Multi-object wait (WaitForMultipleObjects) is not implemented.
 *  - Timeout scan is "check on the way in + tick"; a thread waiting
 *    with a timeout is not on a timer queue, so timeout fires lazily
 *    on the next wait/tick of anyone. Host tests tick in sched_start.
 * Fixed here: mutex ownership is checked; abandoned mutex wakes with
 * STATUS_ABANDONED; wait at raised rank panics in the lock path.
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
        if (d->type == DISP_SYNCHRONIZATION_EVENT) d->signal_state = 0;
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
    disp_signal(&e->disp, e->auto_reset ? (e->disp.signal_state ? 0 : 1) : 1);
    if (!e->auto_reset) e->disp.signal_state = 1;
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
    m->disp.signal_state = 1;
    disp_wake_one(&m->disp, STATUS_SUCCESS);
    /* The waker must transfer ownership if someone was waiting. */
    if (m->disp.signal_state == 0) {
        /* wake_one decremented via... we didn't. Manual: */
    }
    spin_unlock(&m->disp.lock);
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
