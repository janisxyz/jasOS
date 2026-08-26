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
 *  - PI is not a boost chain: only the mutex owner is raised.
 * Fixed here: trampoline always calls sched_exit_thread; idle never
 * exits; init death panics; owner death abandons mutexes; kill_pending
 * is honoured on first run and on every switch-in.
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
    if (t->state == THR_READY || t->state == THR_RUNNING) {
        spin_unlock(&g_sched_lock);
        return;
    }
    t->state = THR_READY;
    ready_insert(t);
    pcb_t *cpu = ke_pcb();
    if (cpu->current && t->priority > cpu->current->priority)
        cpu->need_resched = true;
    spin_unlock(&g_sched_lock);
}

void sched_boost(thread_t *t, u32 new_prio)
{
    if (!t || t == g_idle) return;
    if (new_prio >= PRIORITY_LEVELS) new_prio = PRIORITY_LEVELS - 1;
    spin_lock(&g_sched_lock);
    u32 old = t->priority;
    if (old == new_prio) {
        spin_unlock(&g_sched_lock);
        return;
    }
    if (t->state == THR_READY) {
        list_remove(&t->ready_link);
        if (list_empty(&g_ready[old]))
            g_ready_mask &= ~(1u << old);
        t->priority = new_prio;
        ready_insert(t);
    } else {
        t->priority = new_prio;
    }
    pcb_t *cpu = ke_pcb();
    if (cpu->current && t != cpu->current && t->priority > cpu->current->priority)
        cpu->need_resched = true;
    if (cpu->current == t && t->priority < old)
        cpu->need_resched = true;
    spin_unlock(&g_sched_lock);
}

static void thread_unlink_waits(thread_t *t)
{
    if (t->wait_multi_count) {
        for (u32 i = 0; i < t->wait_multi_count; i++) {
            dispatcher_t *d = t->wait_multi[i].object;
            if (!d) continue;
            spin_lock(&d->lock);
            list_remove(&t->wait_multi[i].obj_link);
            t->wait_multi[i].object = NULL;
            spin_unlock(&d->lock);
        }
        t->wait_multi_count = 0;
    } else if (t->wait.object) {
        dispatcher_t *d = t->wait.object;
        spin_lock(&d->lock);
        list_remove(&t->wait.obj_link);
        t->wait.object = NULL;
        spin_unlock(&d->lock);
    }
}

void sched_kill_thread(thread_t *t, status_t st)
{
    if (!t || t == g_idle) return;
    if (t->state == THR_TERMINATED) return;
    if (t == ke_current()) {
        sched_exit_thread(st);
        return;
    }
    t->kill_pending = true;
    t->exit_status = st;
    if (t->state == THR_WAITING) {
        thread_unlink_waits(t);
        sched_ready(t);
    }
}

#ifdef JASOS_HOST
static jmp_buf g_boot_jmp;
static int     g_boot_valid;
#endif

#ifdef JASOS_HOST
static int live_nonidle(void)
{
    for (u32 i = 0; i < g_nprocs; i++) {
        process_t *p = g_procs[i];
        if (!p) continue;
        u32 live = p->thread_count;
        if (p == g_system && live > 0) live--; /* idle */
        if (live > 0) return 1;
    }
    return 0;
}
#endif

static void idle_entry(void *arg)
{
    (void)arg;
    for (;;) {
#ifdef JASOS_HOST
        ke_on_tick();
        if (!live_nonidle() && g_boot_valid) {
            g_host_stop = true;
            longjmp(g_boot_jmp, 1);
        }
        if (ke_ticks() > 1000000ull) {
            kprintf("sched: host watchdog — abandoning waiters\n");
            g_host_stop = true;
            longjmp(g_boot_jmp, 1);
        }
        sched_yield();
#else
        __asm__ volatile("sti; hlt; cli");
        if (ke_pcb()->need_resched) sched_reschedule();
#endif
    }
}

#ifdef JASOS_HOST
__attribute__((noinline, noclone, used))
void thread_trampoline(void)
{
    thread_t *t = ke_current();
    if (t && t->kill_pending) sched_exit_thread(t->exit_status);
    if (t && t->ctx.entry) t->ctx.entry(t->ctx.arg);
    sched_exit_thread(STATUS_SUCCESS);
}

__attribute__((noinline, noclone))
void context_switch(context_t *old, context_t *newc)
{
    thread_t *t = ke_current();
    if (newc->entry && !newc->valid) {
        if (!t || !t->kstack) panic("context_switch: no kstack");
        if (getcontext(&newc->uc) != 0) panic("getcontext");
        newc->uc.uc_stack.ss_sp = t->kstack;
        newc->uc.uc_stack.ss_size = t->kstack_size;
        newc->uc.uc_link = NULL;
        makecontext(&newc->uc, thread_trampoline, 0);
        newc->valid = 1;
        __asm__ volatile("" ::: "memory");
    }
    if (!newc->valid) panic("context_switch: no frame");
    __asm__ volatile("" ::: "memory");
    if (!old) {
        setcontext(&newc->uc);
        panic("setcontext returned");
    }
    if (swapcontext(&old->uc, &newc->uc) != 0)
        panic("swapcontext");
}
#else
void thread_trampoline(void)
{
    thread_t *t = ke_current();
    if (t && t->kill_pending) sched_exit_thread(t->exit_status);
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
#ifndef JASOS_HOST
    fpu_lazy_switch();
    if (next->kstack) {
        cpu->kernel_rsp = (u64)(next->kstack + next->kstack_size);
        tss_set_rsp0(cpu->kernel_rsp);
    }
    if (next->process && next->process->aspace.cr3_phys) {
        u64 cr3 = next->process->aspace.cr3_phys;
        __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
    }
#endif
    if (cur) context_switch(&cur->ctx, &next->ctx);
    else     context_switch(NULL, &next->ctx);
    thread_t *self = ke_current();
    if (self && self->kill_pending)
        sched_exit_thread(self->exit_status);
}

void sched_yield(void)
{
    sched_reschedule();
}

void sched_exit_thread(status_t st)
{
    thread_t *t = ke_current();
    /* Dying waiter must not stay on an object's wait_list. One DISP
       at a time; then we take SCHED in reschedule. */
    thread_unlink_waits(t);
    ke_mutex_abandon_owned(t);
#ifdef JASOS_HOST
    if (t->kstack) {
        u8 *g = t->kstack - PAGE_SIZE;
        for (u32 i = 0; i < 16; i++) {
            if (g[i] != 0xA5)
                panic("kstack smash thread %s", t->name);
        }
    }
#else
    fpu_drop(t);
    if (t->kstack) {
        vmm_unmap_kstack((u32)t->tid);
        t->kstack = NULL;
    }
#endif
    t->exit_status = st;
    t->state = THR_TERMINATED;
    disp_signal(&t->disp, 1);
    process_t *proc = t->process;
    int last = 0;
    if (proc) {
        spin_lock(&proc->lock);
        proc->thread_count--;
        if (proc->thread_count == 0) {
            proc->exit_status = st;
            proc->terminating = true;
            last = 1;
            if (proc->pid == 1) panic("init died status %s", status_name(st));
        }
        spin_unlock(&proc->lock);
        if (last) {
            disp_signal(&proc->disp, 1);
            /* No PROC lock: VMM=4 and HANDLE=7 are below PROC=8. */
            vmm_aspace_destroy(&proc->aspace);
            ht_destroy(&proc->handles);
            if (proc->pid != 0 && proc->pid != 1) {
                for (u32 i = 0; i < g_nprocs; i++) {
                    if (g_procs[i] == proc) {
                        g_procs[i] = g_procs[--g_nprocs];
                        g_procs[g_nprocs] = NULL;
                        ob_dereference(&proc->hdr); /* drop table ref */
                        break;
                    }
                }
            }

        }
    }
#ifdef JASOS_HOST
    /* Do not setcontext a stale idle frame after a previous longjmp. */
    if (!live_nonidle() && g_boot_valid) {
        g_host_stop = true;
        ke_pcb()->current = g_idle;
        ke_pcb()->current_process = g_system;
        longjmp(g_boot_jmp, 1);
    }
#endif
    ke_pcb()->current = NULL;
    sched_reschedule();
    panic("exited thread resumed");
}

void ke_on_tick(void)
{
    pcb_t *cpu = ke_pcb();
    cpu->ticks++;
    timer_tick(cpu->ticks);

    for (u32 i = 0; i < g_nprocs; i++) {
        process_t *p = g_procs[i];
        if (!p) continue;
        if (p->thread_count == 0) continue;
        if (list_empty(&p->threads)) continue;
        if (!p->threads.next || !p->threads.prev) continue;
        list_t *e, *n;
        LIST_FOR_EACH_SAFE(e, n, &p->threads) {
            if (!e || !e->next || !e->prev) break;
            thread_t *th = CONTAINER_OF(e, thread_t, proc_link);
            if (th->state != THR_WAITING || !th->wait_timed) continue;
            if (cpu->ticks < th->wait_timeout_tick) continue;
            if (th->wait_multi_count) {
                for (u32 k = 0; k < th->wait_multi_count; k++) {
                    dispatcher_t *d = th->wait_multi[k].object;
                    if (!d) continue;
                    spin_lock(&d->lock);
                    list_remove(&th->wait_multi[k].obj_link);
                    spin_unlock(&d->lock);
                }
                th->wait.wake_status = STATUS_TIMEOUT;
                th->wait_timed = false;
                th->wait_multi_count = 0;
                sched_ready(th);
                cpu->need_resched = true;
                continue;
            }
            dispatcher_t *d = th->wait.object;
            if (d) {
                spin_lock(&d->lock);
                if (th->state == THR_WAITING) {
                    list_remove(&th->wait.obj_link);
                    th->wait.wake_status = STATUS_TIMEOUT;
                    th->wait_timed = false;
                }
                spin_unlock(&d->lock);
            }
            if (th->state == THR_WAITING) sched_ready(th);
            cpu->need_resched = true;
        }
    }
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
    spin_init(&g_system->lock, "proc", LOCK_RANK_PROC);
    ht_init(&g_system->handles);
    vmm_aspace_init(&g_system->aspace);
    disp_init(&g_system->disp, DISP_PROCESS, 0);
    g_system->hdr.wait = &g_system->disp;
    strlcpy(g_system->cwd, "/", PATH_MAX);
    {
        object_t *tok = ob_create(ob_type_token(), NULL, NULL);
        if (!tok) panic("sched: system token");
        token_object_t *t = (token_object_t *)tok;
        t->pid = g_system->pid;
        t->integrity = 1; /* admin until logon — do not claim otherwise */
        g_system->token = t;
    }
    g_procs[g_nprocs++] = g_system;

    status_t st = psp_create_thread(g_system, "idle", idle_entry, NULL, PRIORITY_IDLE, 0, &g_idle);
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
    kprintf("sched: dropping boot context\n");
#ifdef JASOS_HOST
    g_host_stop = false;
    g_idle->ctx.valid = 0;
    if (g_idle->state == THR_RUNNING)
        g_idle->state = THR_READY;
    if (setjmp(g_boot_jmp) != 0) {
        ke_pcb()->current = g_idle;
        ke_pcb()->current_process = g_system;
        kprintf("sched: host idle — back to boot\n");
        return;
    }
    g_boot_valid = 1;
#endif
    ke_pcb()->current = NULL;
    sched_reschedule();
#ifdef JASOS_HOST
    /* If reschedule never longjmp'd (no idle path), tick until it does. */
    u32 guard = 0;
    while (!g_host_stop && guard++ < 100000) ke_on_tick();
#endif
}

status_t psp_create_thread(process_t *p, const char *name, void (*entry)(void *),
                           void *arg, u32 prio, u32 flags, thread_t **out)
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
    t->kill_pending = false;
    t->wait_boost = 0;
    list_init(&t->owned_mutexes);
    t->kstack_size = KSTACK_SIZE;
#ifdef JASOS_HOST
    {
        void *raw = NULL;
        if (posix_memalign(&raw, 16, KSTACK_SIZE + PAGE_SIZE) != 0)
            t->kstack = NULL;
        else {
            memset(raw, 0xA5, PAGE_SIZE);
            memset((u8 *)raw + PAGE_SIZE, 0, KSTACK_SIZE);
            t->kstack = (u8 *)raw + PAGE_SIZE;
        }
    }
#else
    {
        status_t ks = vmm_map_kstack((u32)t->tid, &t->kstack);
        if (!NT_SUCCESS(ks)) t->kstack = NULL;
        t->fpu_used = false;
    }
#endif
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
    if (!(flags & CREATE_SUSPENDED))
        sched_ready(t);
    return STATUS_SUCCESS;
}

status_t psp_create_process(const char *image, process_t *parent, process_t **out)
{
    process_t *p = (process_t *)ob_create(ob_type_process(), image, NULL);
    if (!p) return STATUS_NO_MEMORY;
    p->pid = g_next_pid++;
    list_init(&p->threads);
    spin_init(&p->lock, "proc", LOCK_RANK_PROC);
    ht_init(&p->handles);
    vmm_aspace_init(&p->aspace);
    disp_init(&p->disp, DISP_PROCESS, 0);
    p->hdr.wait = &p->disp;
    strlcpy(p->image, image ? image : "unknown", sizeof(p->image));
    if (parent) strlcpy(p->cwd, parent->cwd, PATH_MAX);
    else strlcpy(p->cwd, "/", PATH_MAX);
    {
        object_t *tok = ob_create(ob_type_token(), NULL, NULL);
        if (!tok) {
            /* Hardware aspace_init already took a CR3. Fail closed. */
            vmm_aspace_destroy(&p->aspace);
            ob_dereference(&p->hdr);
            return STATUS_NO_MEMORY;
        }
        token_object_t *t = (token_object_t *)tok;
        t->pid = p->pid;
        t->integrity = 1; /* admin until logon exists — do not claim otherwise */
        p->token = t;
    }
    if (g_nprocs < MAX_PROCESSES) {
        g_procs[g_nprocs++] = p;
        ob_reference(&p->hdr); /* table ref; NtClose must not free us */
    }
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
