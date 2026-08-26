#pragma once

#include <jasos/types.h>
#include <jasos/config.h>
#include <jasos/status.h>
#include <jasos/ob.h>
#include <jasos/mm.h>

typedef enum thread_state {
    THR_UNUSED = 0,
    THR_READY,
    THR_RUNNING,
    THR_WAITING,
    THR_TERMINATED
} thread_state_t;

#ifdef JASOS_HOST
typedef struct context {
    jmp_buf buf;
    int     valid;
    void  (*entry)(void *);
    void   *arg;
} context_t;
#else
typedef struct context {
    u64 r15, r14, r13, r12, rbx, rbp;
    u64 rip, rsp, rflags;
} context_t;
#endif

typedef struct trap_frame {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rdi, rsi, rbp, rbx, rdx, rcx, rax;
    u64 rip, cs, rflags, rsp, ss;
    u64 error, vector, cr2;
} trap_frame_t;

struct file_object;
struct vnode;

typedef struct thread {
    object_t          hdr;
    dispatcher_t      disp;
    struct process   *process;
    ktid_t            tid;
    thread_state_t    state;
    u32               priority;
    u32               saved_priority;
    u32               quantum_left;
    u32               wait_boost;
    context_t         ctx;
    u8               *kstack;
    usize             kstack_size;
    trap_frame_t     *tf;
    wait_block_t      wait;
    u64               wait_timeout_tick;
    bool              wait_timed;
    list_t            ready_link;
    list_t            proc_link;
    list_t            timer_link;
    status_t          exit_status;
    char              name[32];
    /* syscall scratch */
    status_t          last_status;
} thread_t;

typedef struct process {
    object_t       hdr;
    dispatcher_t   disp;
    kpid_t         pid;
    aspace_t       aspace;
    handle_table_t handles;
    thread_t      *primary;
    list_t         threads;
    spinlock_t     lock;
    status_t       exit_status;
    bool           terminating;
    char           image[64];
    char           cwd[PATH_MAX];
    u32            thread_count;
    handle_t       std_in, std_out, std_err;
} process_t;

typedef struct pcb {
    thread_t *current;
    process_t *current_process;
    thread_t *idle;
    u32       irql;
    u32       held_rank;
    bool      need_resched;
    u64       ticks;
} pcb_t;

void      ke_init(void);
pcb_t    *ke_pcb(void);
thread_t *ke_current(void);
process_t *ke_current_process(void);

void      sched_init(void);
void      sched_start(void); /* does not return on hardware; returns on host when idle+no ready */
void      sched_ready(thread_t *t);
void      sched_yield(void);
void      sched_reschedule(void);
void      sched_exit_thread(status_t st);
void      ke_on_tick(void);
u64       ke_ticks(void);

status_t  psp_create_process(const char *image, process_t *parent, process_t **out);
status_t  psp_create_thread(process_t *p, const char *name, void (*entry)(void *), void *arg, u32 prio, thread_t **out);
status_t  psp_create_init(void);
process_t *psp_system_process(void);

status_t  ke_wait_object(dispatcher_t *d, u64 timeout_ticks);
status_t  ke_set_event(event_object_t *e);
status_t  ke_reset_event(event_object_t *e);
status_t  ke_release_mutex(mutex_object_t *m);
status_t  ke_acquire_mutex(mutex_object_t *m, u64 timeout_ticks);

void      context_switch(context_t *old, context_t *newc);
void      thread_trampoline(void);

void      gdt_init(void);
void      idt_init(void);
void      tss_init(void);
void      pic_remap(u8 off1, u8 off2);
void      pic_unmask(u8 irq);
void      pit_init(u32 hz);
void      syscall_init(void);

extern volatile bool g_panic_in_progress;
extern volatile bool g_sched_started;
