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

/*
 * WaitForMultipleObjects. Enqueues a wait_block on every object so a
 * signal on any of them wakes us (WAIT_ANY) or we recheck after each
 * wake for WAIT_ALL. Never holds two DISP locks at once — enqueue
 * one object at a time. Double-wake is absorbed by sched_ready.
 *
 * Why this will fail in production:
 *  - WAIT_ALL consumes objects in a second pass with timeout 0; a
 *    concurrent waiter can steal a sync event between the wake and
 *    the consume. NT has the same class of race without a rundown.
 *  - 16-object cap. A 17th handle fails closed.
 */
status_t ke_wait_multiple(dispatcher_t **objs, u32 count, bool wait_all, u64 timeout_ticks)
{
    if (!objs || count == 0 || count > WAIT_OBJECTS_MAX)
        return STATUS_INVALID_PARAMETER;
    thread_t *t = ke_current();
    if (!t) return STATUS_INVALID_PARAMETER;

    for (u32 i = 0; i < count; i++)
        if (!objs[i]) return STATUS_INVALID_PARAMETER;

    /* Fast path: already satisfied. One DISP at a time. */
    if (!wait_all) {
        for (u32 i = 0; i < count; i++) {
            status_t st = ke_wait_object(objs[i], 0);
            if (st != STATUS_TIMEOUT)
                return (st == STATUS_SUCCESS) ? (status_t)i : st;
        }
    } else {
        u32 got = 0;
        for (u32 i = 0; i < count; i++) {
            spin_lock(&objs[i]->lock);
            if (objs[i]->signal_state > 0) got++;
            spin_unlock(&objs[i]->lock);
        }
        if (got == count) {
            for (u32 i = 0; i < count; i++)
                ke_wait_object(objs[i], 0);
            return STATUS_SUCCESS;
        }
    }
    if (timeout_ticks == 0) return STATUS_TIMEOUT;

    t->wait_multi_count = count;
    t->wait_all = wait_all;
    t->wait.wake_status = STATUS_TIMEOUT;
    t->wait.thread = t;
    t->wait.object = objs[0];

    for (u32 i = 0; i < count; i++) {
        t->wait_multi[i].thread = t;
        t->wait_multi[i].object = objs[i];
        t->wait_multi[i].wake_status = STATUS_TIMEOUT;
        list_init(&t->wait_multi[i].obj_link);
        list_init(&t->wait_multi[i].thr_link);
        spin_lock(&objs[i]->lock);
        if (!wait_all && objs[i]->signal_state > 0) {
            spin_unlock(&objs[i]->lock);
            for (u32 j = 0; j < i; j++) {
                spin_lock(&objs[j]->lock);
                list_remove(&t->wait_multi[j].obj_link);
                spin_unlock(&objs[j]->lock);
            }
            t->wait_multi_count = 0;
            t->wait.object = NULL;
            /* consume the signaled one */
            ke_wait_object(objs[i], 0);
            return (status_t)i;
        }
        list_insert_tail(&objs[i]->wait_list, &t->wait_multi[i].obj_link);
        spin_unlock(&objs[i]->lock);
    }

    t->state = THR_WAITING;
    t->wait_timed = (timeout_ticks != (u64)-1);
    t->wait_timeout_tick = ke_ticks() + (timeout_ticks == (u64)-1 ? 0 : timeout_ticks);
    sched_reschedule();

    u32 which = 0;
    for (u32 i = 0; i < count; i++) {
        spin_lock(&objs[i]->lock);
        list_remove(&t->wait_multi[i].obj_link);
        if (t->wait_multi[i].wake_status != STATUS_TIMEOUT)
            which = i;
        t->wait_multi[i].object = NULL;
        spin_unlock(&objs[i]->lock);
    }
    t->wait_multi_count = 0;
    t->wait.object = NULL;

    if (t->wait.wake_status == STATUS_TIMEOUT)
        return STATUS_TIMEOUT;
    if (wait_all) {
        for (u32 i = 0; i < count; i++)
            ke_wait_object(objs[i], 0);
        return STATUS_SUCCESS;
    }
    return (status_t)which;
}
