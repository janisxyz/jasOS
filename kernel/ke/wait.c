#include <jasos/ke.h>
#include <jasos/kprintf.h>
#include <jasos/status.h>

/*
 * Wait/signal. Sleeps. Not a spinlock in a trench coat.
 *
 * Why this will fail in production:
 *  - WAIT_ALL consumes objects in a second pass with timeout 0; a
 *    concurrent waiter can steal a sync event between the wake and
 *    the consume. NT has the same class of race without a rundown.
 *  - 16-object cap. A 17th handle fails closed.
 *  - PI is single-donation, not a full boost chain. A waiter of a
 *    waiter of a mutex owner does not propagate. Documented.
 * Fixed here: mutex ownership transfers in disp_wake_one; abandoned
 * mutex wakes with STATUS_ABANDONED; wait never holds DISP across
 * a SCHED acquire without the T3 ranking (DISP=9 < SCHED=10);
 * WaitForMultiple enqueues a wait_block on every object;
 * owner death abandons every mutex on the thread's owned list;
 * a higher-priority waiter donates to the mutex owner (sched_boost);
 * WAIT_ALL treats a mutex the caller already owns as satisfied
 * (signal_state is 0 while held; polling used to TIMEOUT).
 */

void ke_mutex_own(mutex_object_t *m, thread_t *t)
{
    if (!m || !t) return;
    list_remove(&m->owned_link);
    list_insert_tail(&t->owned_mutexes, &m->owned_link);
}

void ke_mutex_disown(mutex_object_t *m)
{
    if (!m) return;
    list_remove(&m->owned_link);
}

void ke_mutex_abandon_owned(thread_t *t)
{
    if (!t) return;
    while (!list_empty(&t->owned_mutexes)) {
        mutex_object_t *m = CONTAINER_OF(t->owned_mutexes.next, mutex_object_t, owned_link);
        list_remove(&m->owned_link);
        spin_lock(&m->disp.lock);
        m->owner = NULL;
        m->recursion = 0;
        m->abandoned = true;
        if (!list_empty(&m->disp.wait_list)) {
            m->disp.signal_state = 0;
            disp_wake_one(&m->disp, STATUS_ABANDONED);
        } else {
            m->disp.signal_state = 1;
        }
        spin_unlock(&m->disp.lock);
    }
}

static void mutex_donate(mutex_object_t *m, thread_t *waiter)
{
    thread_t *owner = m->owner;
    if (!owner || !waiter || owner == waiter) return;
    if (waiter->priority <= owner->priority) return;
    if (!owner->wait_boost)
        owner->saved_priority = owner->priority;
    owner->wait_boost = 1;
    sched_boost(owner, waiter->priority);
}

static int disp_satisfied(dispatcher_t *d, thread_t *t)
{
    if (!d) return 0;
    if (d->type == DISP_MUTANT) {
        mutex_object_t *m = CONTAINER_OF(d, mutex_object_t, disp);
        if (m->owner == t) return 1;
    }
    return d->signal_state > 0;
}

status_t ke_wait_object(dispatcher_t *d, u64 timeout_ticks)
{
    if (!d) return STATUS_INVALID_PARAMETER;
    thread_t *t = ke_current();
    if (!t) return STATUS_INVALID_PARAMETER;
    if (t->kill_pending)
        sched_exit_thread(t->exit_status);

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
            ke_mutex_own(m, t);
            spin_unlock(&d->lock);
            return st;
        }
        mutex_donate(m, t);
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
    if (t->kill_pending)
        sched_exit_thread(t->exit_status);
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
    ke_mutex_disown(m);
    if (list_empty(&m->disp.wait_list)) {
        m->disp.signal_state = 1;
        spin_unlock(&m->disp.lock);
    } else {
        /* Transfer while still holding DISP; wake_one takes SCHED. */
        disp_wake_one(&m->disp, STATUS_SUCCESS);
        spin_unlock(&m->disp.lock);
    }
    if (list_empty(&t->owned_mutexes) && t->wait_boost) {
        t->wait_boost = 0;
        sched_boost(t, t->saved_priority);
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
    if (t->kill_pending)
        sched_exit_thread(t->exit_status);

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
            if (disp_satisfied(objs[i], t)) got++;
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
        if (objs[i]->type == DISP_MUTANT) {
            mutex_object_t *m = CONTAINER_OF(objs[i], mutex_object_t, disp);
            mutex_donate(m, t);
        }
        list_insert_tail(&objs[i]->wait_list, &t->wait_multi[i].obj_link);
        spin_unlock(&objs[i]->lock);
    }

    t->state = THR_WAITING;
    t->wait_timed = (timeout_ticks != (u64)-1);
    t->wait_timeout_tick = ke_ticks() + (timeout_ticks == (u64)-1 ? 0 : timeout_ticks);
    sched_reschedule();

    if (t->kill_pending)
        sched_exit_thread(t->exit_status);

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