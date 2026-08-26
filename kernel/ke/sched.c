#include <jasos/ke.h>
#include <jasos/kprintf.h>
#include <jasos/string.h>
#include <jasos/mm.h>
#include <jasos/syscall.h>
#include <jasos/status.h>



/*
 * Dispatcher. UP, 32-level RR, PIT quantum.
 *
 * Why this will fail in production:
 *  - Host context switch uses setjmp; a thread that returns from its
 *    entry without calling sched_exit_thread longjmps into freed stack.
 *  - No affinity, no SMT, no load balancer.
 *  - FXSAVE missing (documented in SCHEDULER.md).
 * Fixed here: trampoline always calls sched_exit_thread; idle never
 * exits; init death panics.
 */

static list_t     g_ready[PRIORITY_LEVELS];
static u32        g_ready_mask;
static spinlock_t g_sched_lock = SPINLOCK_INIT("sched", LOCK_RANK_SCHED);
static thread_t  *g_idle;
static process_t *g_system;
static kpid_t     g_next_pid = 1;
static ktid_t     g_next_tid = 1;
static process_t *g_procs[MAX_PROCESSES];
static u32        g_nprocs;
#ifdef JASOS_HOST
static bool       g_host_stop;
#endif

process_t *psp_system_process(void) { return g_system; }

static void ready_insert(thread_t *t)
{
    list_insert_tail(&g_ready[t->priority], &t->ready_link);
    g_ready_mask |= (1u << t->priority);
}

static thread_t *ready_pop_highest(void)
{
    if (!g_ready_mask) return NULL;
    u32 prio = 31u - (u32)__builtin_clz(g_ready_mask);
    list_t *h = &g_ready[prio];
    if (list_empty(h)) {
        g_ready_mask &= ~(1u << prio);
        return NULL;
    }
    list_t *e = h->next;
    list_remove(e);
    if (list_empty(h)) g_ready_mask &= ~(1u << prio);
    return CONTAINER_OF(e, thread_t, ready_link);
}

void sched_ready(thread_t *t)
{
    if (!t || t->state == THR_TERMINATED) return;
    spin_lock(&g_sched_lock);
    t->state = THR_READY;
    ready_insert(t);
    pcb_t *cpu = ke_pcb();
    if (cpu->current && t->priority > cpu->current->priority)
        cpu->need_resched = true;
    spin_unlock(&g_sched_lock);
}

static void idle_entry(void *arg)
{
    (void)arg;
    for (;;) {
#ifdef JASOS_HOST
        if (!g_ready_mask) {
            g_host_stop = true;
            return;
        }
        sched_yield();
#else
        __asm__ volatile("sti; hlt; cli");
        if (ke_pcb()->need_resched) sched_reschedule();
#endif
    }
}

#ifdef JASOS_HOST
void thread_trampoline(void)
{
    thread_t *t = ke_current();
    if (t->ctx.entry) t->ctx.entry(t->ctx.arg);
    sched_exit_thread(STATUS_SUCCESS);
}

void context_switch(context_t *old, context_t *newc)
{
    if (old) {
        if (setjmp(old->buf) != 0) return; /* resumed */
        old->valid = 1;
    }
    if (newc->entry && !newc->valid) {
        newc->valid = 1;
        thread_trampoline();
        return;
    }
    if (newc->valid) longjmp(newc->buf, 1);
}
#else
void thread_trampoline(void)
{
    thread_t *t = ke_current();
    void (*fn)(void *) = (void (*)(void *))t->ctx.rbx;
    void *arg = (void *)t->ctx.r12;
    fn(arg);
    sched_exit_thread(STATUS_SUCCESS);
}
#endif

void sched_reschedule(void)
{
    pcb_t *cpu = ke_pcb();
    spin_lock(&g_sched_lock);
    cpu->need_resched = false;
    thread_t *cur = cpu->current;
    thread_t *next = ready_pop_highest();
    if (!next) next = g_idle;
    if (next == cur) {
        spin_unlock(&g_sched_lock);
        return;
    }
    if (cur && cur->state == THR_RUNNING) {
        cur->state = THR_READY;
        if (cur != g_idle) ready_insert(cur);
    }
    next->state = THR_RUNNING;
    next->quantum_left = (next->priority >= PRIORITY_REALTIME) ? 0xFFFFu : QUANTUM_TICKS;
    cpu->current = next;
    cpu->current_process = next->process;
    spin_unlock(&g_sched_lock);
    if (cur) context_switch(&cur->ctx, &next->ctx);
    else     context_switch(NULL, &next->ctx);
}

void sched_yield(void)
{
    sched_reschedule();
}

void sched_exit_thread(status_t st)
{
    thread_t *t = ke_current();
    t->exit_status = st;
    t->state = THR_TERMINATED;
    disp_signal(&t->disp, 1);
    if (t->process) {
        spin_lock(&t->process->lock);
        t->process->thread_count--;
        if (t->process->thread_count == 0) {
            t->process->exit_status = st;
            t->process->terminating = true;
            disp_signal(&t->process->disp, 1);
            if (t->process->pid == 1) panic("init died status %s", status_name(st));
        }
        spin_unlock(&t->process->lock);
    }
    ke_pcb()->current = NULL;
    sched_reschedule();
    panic("exited thread resumed");
}

void ke_on_tick(void)
{
    pcb_t *cpu = ke_pcb();
    cpu->ticks++;
    thread_t *t = cpu->current;
    if (!t) return;
    if (t->quantum_left && t->priority < PRIORITY_REALTIME) {
        t->quantum_left--;
        if (t->quantum_left == 0) cpu->need_resched = true;
    }
    if (cpu->need_resched) sched_reschedule();
}

void sched_init(void)
{
    for (u32 i = 0; i < PRIORITY_LEVELS; i++) list_init(&g_ready[i]);
    g_ready_mask = 0;

    g_system = kalloc_zero(sizeof(*g_system));
    if (!g_system) panic("sched: system");
    g_system->hdr.type = ob_type_process();
    g_system->hdr.pointer_count = 1;
    g_system->hdr.flags = OBJ_PERMANENT | OBJ_WAITABLE;
    strlcpy(g_system->hdr.name, "System", NAME_MAX);
    strlcpy(g_system->image, "System", sizeof(g_system->image));
    g_system->pid = 0;
    list_init(&g_system->threads);
    spin_init(&g_system->lock, "proc", LOCK_RANK_SCHED);
    ht_init(&g_system->handles);
    vmm_aspace_init(&g_system->aspace);
    disp_init(&g_system->disp, DISP_PROCESS, 0);
    g_system->hdr.wait = &g_system->disp;
    strlcpy(g_system->cwd, "/", PATH_MAX);
    g_procs[g_nprocs++] = g_system;

    status_t st = psp_create_thread(g_system, "idle", idle_entry, NULL, PRIORITY_IDLE, &g_idle);
    if (!NT_SUCCESS(st)) panic("sched: idle");
    g_idle->state = THR_READY;
    ke_pcb()->idle = g_idle;
    ke_pcb()->current_process = g_system;
    kprintf("sched: system pid 0, idle tid %llu, %u Hz\n",
            (unsigned long long)g_idle->tid, TIMER_HZ);
}

void sched_start(void)
{
    g_sched_started = true;
    ke_pcb()->current = NULL;
    kprintf("sched: dropping boot context\n");
    sched_reschedule();
#ifdef JASOS_HOST
    /* Cooperative: keep switching until idle sees an empty ready mask. */
    u32 guard = 0;
    while (!g_host_stop && guard++ < 100000) {
        ke_on_tick();
        if (!ke_pcb()->current) break;
    }
#endif
}

status_t psp_create_thread(process_t *p, const char *name, void (*entry)(void *),
                           void *arg, u32 prio, thread_t **out)
{
    if (!p || !entry) return STATUS_INVALID_PARAMETER;
    if (prio >= PRIORITY_LEVELS) prio = PRIORITY_NORMAL;
    thread_t *t = (thread_t *)ob_create(ob_type_thread(), name, NULL);
    if (!t) return STATUS_NO_MEMORY;
    t->process = p;
    t->tid = g_next_tid++;
    t->state = THR_UNUSED;
    t->priority = prio;
    t->saved_priority = prio;
    t->quantum_left = QUANTUM_TICKS;
    t->kstack_size = KSTACK_SIZE;
    t->kstack = kalloc_zero(KSTACK_SIZE);
    if (!t->kstack) {
        ob_dereference(&t->hdr);
        return STATUS_NO_MEMORY;
    }
    disp_init(&t->disp, DISP_THREAD, 0);
    t->hdr.wait = &t->disp;
    list_init(&t->ready_link);
    list_init(&t->proc_link);
    list_init(&t->timer_link);
    if (name) strlcpy(t->name, name, sizeof(t->name));

#ifdef JASOS_HOST
    t->ctx.valid = 0;
    t->ctx.entry = entry;
    t->ctx.arg = arg;
#else
    u64 *sp = (u64 *)(t->kstack + KSTACK_SIZE);
    sp = (u64 *)((u64)sp & ~0xFULL);
    t->ctx.rsp = (u64)sp;
    t->ctx.rip = (u64)thread_trampoline;
    t->ctx.rflags = 0x202;
    t->ctx.rbx = (u64)entry;
    t->ctx.r12 = (u64)arg;
#endif
    spin_lock(&p->lock);
    list_insert_tail(&p->threads, &t->proc_link);
    p->thread_count++;
    if (!p->primary) p->primary = t;
    spin_unlock(&p->lock);
    ob_reference(&p->hdr);
    if (out) *out = t;
    sched_ready(t);
    return STATUS_SUCCESS;
}

status_t psp_create_process(const char *image, process_t *parent, process_t **out)
{
    process_t *p = (process_t *)ob_create(ob_type_process(), image, NULL);
    if (!p) return STATUS_NO_MEMORY;
    p->pid = g_next_pid++;
    list_init(&p->threads);
    spin_init(&p->lock, "proc", LOCK_RANK_SCHED);
    ht_init(&p->handles);
    vmm_aspace_init(&p->aspace);
    disp_init(&p->disp, DISP_PROCESS, 0);
    p->hdr.wait = &p->disp;
    strlcpy(p->image, image ? image : "unknown", sizeof(p->image));
    if (parent) strlcpy(p->cwd, parent->cwd, PATH_MAX);
    else strlcpy(p->cwd, "/", PATH_MAX);
    if (g_nprocs < MAX_PROCESSES) g_procs[g_nprocs++] = p;
    if (out) *out = p;
    return STATUS_SUCCESS;
}

/* Used by NtQuerySystemInformation class 5 */
u32 psp_snapshot(sys_process_info_t *buf, u32 max)
{
    u32 n = 0;
    for (u32 i = 0; i < g_nprocs && n < max; i++) {
        process_t *p = g_procs[i];
        if (!p) continue;
        buf[n].pid = p->pid;
        buf[n].threads = p->thread_count;
        strlcpy(buf[n].image, p->image, sizeof(buf[n].image));
        strlcpy(buf[n].state, p->terminating ? "exit" : "run", sizeof(buf[n].state));
        n++;
    }
    return n;
}

void wait_for_object_timeout_scan(void)
{
    /* v1: timeouts are tick-granular and checked in ke_wait via spin. */
}
