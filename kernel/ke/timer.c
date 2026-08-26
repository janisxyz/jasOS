#include <jasos/ke.h>
#include <jasos/kprintf.h>
#include <jasos/string.h>
#include <jasos/status.h>

/*
 * Kernel timers. Waitable dispatcher objects on a global due-list.
 * PIT (or host idle) calls timer_tick every quantum.
 *
 * Why this will fail in production:
 *  - No DPC; the waker runs in the ticker's context.
 *  - Coalescing is linear in armed timers.
 *  - Period re-arm does not account for overrun (we skip missed ticks).
 */

static list_t     g_timers;
static spinlock_t g_timer_lock = SPINLOCK_INIT("timerq", LOCK_RANK_PROC);
static bool       g_timer_up;

void timer_init(void)
{
    list_init(&g_timers);
    g_timer_up = true;
}

status_t ob_create_timer(const char *name, bool auto_reset, timer_object_t **out)
{
    if (!out) return STATUS_INVALID_PARAMETER;
    object_t *o = ob_create(ob_type_timer(), name, name ? ob_dir_bno() : NULL);
    if (!o) return STATUS_NO_MEMORY;
    timer_object_t *t = (timer_object_t *)o;
    disp_init(&t->disp, auto_reset ? DISP_SYNCHRONIZATION_EVENT : DISP_NOTIFICATION_EVENT, 0);
    o->wait = &t->disp;
    list_init(&t->timer_link);
    t->armed = false;
    t->period = 0;
    t->due_tick = 0;
    *out = t;
    return STATUS_SUCCESS;
}

status_t ke_set_timer(timer_object_t *t, u64 due_ticks, u64 period)
{
    if (!t) return STATUS_INVALID_PARAMETER;
    if (!g_timer_up) timer_init();
    spin_lock(&g_timer_lock);
    if (t->armed) {
        list_remove(&t->timer_link);
        t->armed = false;
    }
    t->due_tick = ke_ticks() + (due_ticks ? due_ticks : 1);
    t->period = period;
    t->armed = true;
    t->disp.signal_state = 0;
    list_insert_tail(&g_timers, &t->timer_link);
    spin_unlock(&g_timer_lock);
    return STATUS_SUCCESS;
}

status_t ke_cancel_timer(timer_object_t *t)
{
    if (!t) return STATUS_INVALID_PARAMETER;
    spin_lock(&g_timer_lock);
    if (t->armed) {
        list_remove(&t->timer_link);
        t->armed = false;
    }
    spin_unlock(&g_timer_lock);
    spin_lock(&t->disp.lock);
    t->disp.signal_state = 0;
    spin_unlock(&t->disp.lock);
    return STATUS_SUCCESS;
}

void timer_tick(u64 now)
{
    if (!g_timer_up) return;
    for (;;) {
        timer_object_t *due = NULL;
        spin_lock(&g_timer_lock);
        list_t *e, *n;
        LIST_FOR_EACH_SAFE(e, n, &g_timers) {
            timer_object_t *t = CONTAINER_OF(e, timer_object_t, timer_link);
            if (t->armed && now >= t->due_tick) {
                list_remove(&t->timer_link);
                t->armed = false;
                due = t;
                break;
            }
        }
        spin_unlock(&g_timer_lock);
        if (!due) return;
        disp_signal(&due->disp, 1);
        if (due->period) {
            spin_lock(&g_timer_lock);
            due->due_tick = now + due->period;
            due->armed = true;
            list_insert_tail(&g_timers, &due->timer_link);
            spin_unlock(&g_timer_lock);
        }
    }
}

status_t NtCreateTimer(handle_t *out, const char *name, bool auto_reset)
{
    if (!out) return STATUS_INVALID_PARAMETER;
    timer_object_t *t;
    status_t st = ob_create_timer(name, auto_reset, &t);
    if (!NT_SUCCESS(st)) return st;
    process_t *p = ke_current_process();
    if (!p) {
        ob_dereference(&t->hdr);
        return STATUS_INVALID_HANDLE;
    }
    st = ht_insert(&p->handles, &t->hdr, SYNCHRONIZE | TIMER_MODIFY_STATE, out);
    ob_dereference(&t->hdr);
    return st;
}

status_t NtSetTimer(handle_t h, u64 due_ticks, u64 period)
{
    object_t *o;
    process_t *p = ke_current_process();
    if (!p) return STATUS_INVALID_HANDLE;
    status_t st = ht_lookup(&p->handles, h, TIMER_MODIFY_STATE, OBJ_TIMER, &o);
    if (!NT_SUCCESS(st)) return st;
    st = ke_set_timer((timer_object_t *)o, due_ticks, period);
    ob_dereference(o);
    return st;
}

status_t NtCancelTimer(handle_t h)
{
    object_t *o;
    process_t *p = ke_current_process();
    if (!p) return STATUS_INVALID_HANDLE;
    status_t st = ht_lookup(&p->handles, h, TIMER_MODIFY_STATE, OBJ_TIMER, &o);
    if (!NT_SUCCESS(st)) return st;
    st = ke_cancel_timer((timer_object_t *)o);
    ob_dereference(o);
    return st;
}
