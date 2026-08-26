#include <jasos/kprintf.h>
#include <jasos/ke.h>
#include <jasos/string.h>

volatile bool g_panic_in_progress = false;
volatile bool g_sched_started     = false;

static pcb_t g_bsp;

pcb_t *ke_pcb(void)
{
    return &g_bsp;
}

thread_t *ke_current(void)
{
    return g_bsp.current;
}

process_t *ke_current_process(void)
{
    return g_bsp.current_process;
}

u64 ke_ticks(void)
{
    return g_bsp.ticks;
}

void ke_init(void)
{
    memset(&g_bsp, 0, sizeof(g_bsp));
    g_bsp.irql = IRQL_PASSIVE;
}

static void halt_forever(void) NORETURN;

static void halt_forever(void)
{
#ifdef JASOS_HOST
    abort();
#else
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
#endif
}

void NORETURN panic(const char *fmt, ...)
{
    va_list ap;
    g_panic_in_progress = true;
    kprintf("\n*** AEGIS PANIC ***\n");
    if (g_bsp.current) {
        kprintf("thread %llu (%s) pid %llu\n",
                (unsigned long long)g_bsp.current->tid,
                g_bsp.current->name,
                g_bsp.current_process ? (unsigned long long)g_bsp.current_process->pid : 0ULL);
    }
    kprintf("ticks %llu irql %u held_rank %u depth %u\n",
            (unsigned long long)g_bsp.ticks, g_bsp.irql,
            g_bsp.held_rank, g_bsp.held_depth);
    if (g_bsp.held_depth) {
        kprintf("rank stack:");
        for (u32 i = 0; i < g_bsp.held_depth && i < LOCK_DEPTH_MAX; i++)
            kprintf(" %u", g_bsp.rank_stack[i]);
        kprintf("\n");
    }
    kprintf("why: ");
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    kprintf("\n*** halt ***\n");
    halt_forever();
}

void NORETURN panic_with_regs(const char *why, const u64 *gprs)
{
    static const char *names[] = {
        "rax","rbx","rcx","rdx","rsi","rdi","rbp","rsp",
        "r8 ","r9 ","r10","r11","r12","r13","r14","r15",
        "rip","rfl","cr2","cs "
    };
    g_panic_in_progress = true;
    kprintf("\n*** AEGIS PANIC *** %s\n", why ? why : "regs");
    if (gprs) {
        for (u32 i = 0; i < 20; i++) {
            kprintf("  %s %016llx%s", names[i],
                    (unsigned long long)gprs[i],
                    (i % 2) ? "\n" : "  ");
        }
    }
    panic("%s", why ? why : "exception");
}
