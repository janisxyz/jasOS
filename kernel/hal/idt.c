#include <jasos/ke.h>
#include <jasos/kprintf.h>
#include <jasos/string.h>
#include <jasos/mm.h>

typedef struct PACKED {
    u16 off_low;
    u16 sel;
    u8  ist;
    u8  type;
    u16 off_mid;
    u32 off_high;
    u32 zero;
} idt_entry_t;

typedef struct PACKED {
    u16 len;
    u64 ptr;
} idtr_t;

static idt_entry_t idt[256];
static idtr_t      idtr;

extern u64 isr_stub_table[34];
extern void isr_stub_generic(void);

static const char *exc_name(u64 v)
{
    static const char *names[32] = {
        "#DE","#DB","NMI","#BP","#OF","#BR","#UD","#NM",
        "#DF","cso","#TS","#NP","#SS","#GP","#PF","spu",
        "#MF","#AC","#MC","#XM","#VE","#CP","22","23",
        "24","25","26","27","#HV","#VC","#SX","31"
    };
    if (v < 32) return names[v];
    return "irq/exc";
}

static void idt_set(int v, void (*fn)(void), u8 ist)
{
    u64 a = (u64)fn;
    idt[v].off_low  = (u16)a;
    idt[v].sel      = 0x08;
    idt[v].ist      = ist;
    idt[v].type     = 0x8E; /* interrupt gate, IF cleared */
    idt[v].off_mid  = (u16)(a >> 16);
    idt[v].off_high = (u32)(a >> 32);
    idt[v].zero     = 0;
}

void idt_init(void)
{
    memset(idt, 0, sizeof(idt));
    /* IST4 = general IRQ/exc stack so kernel ticks push a full iretq frame. */
    for (int i = 0; i < 256; i++) idt_set(i, isr_stub_generic, 4);
    for (int i = 0; i < 34; i++) {
        u8 ist = 4;
        if (i == 8) ist = 1;       /* #DF */
        else if (i == 2) ist = 2;  /* NMI */
        else if (i == 18) ist = 3; /* #MC */
        idt_set(i, (void (*)(void))isr_stub_table[i], ist);
    }
    idtr.len = sizeof(idt) - 1;
    idtr.ptr = (u64)idt;
    __asm__ volatile("lidt %0" :: "m"(idtr));
    kprintf("idt: 256 gates IST1=#DF IST2=NMI IST3=#MC IST4=irq\n");
}

static int from_user(const trap_frame_t *tf)
{
    return (tf->cs & 3u) == 3u;
}

void isr_dispatch(trap_frame_t *tf)
{
    u64 vector = tf->vector;

    if (vector == 32) {
        ke_on_tick();
        pic_eoi(0);
        return;
    }
    if (vector == 33) {
        kbd_isr();
        pic_eoi(1);
        return;
    }

    u64 cr2 = 0;
    if (vector == 14)
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

    if (vector == 14 && from_user(tf)) {
        process_t *p = ke_current_process();
        bool wr = (tf->error & 2) != 0;
        if (p && vmm_handle_user_fault(&p->aspace, cr2, wr))
            return;
        kprintf("user #PF cr2=%llx err=%llx — %s\n",
                (unsigned long long)cr2,
                (unsigned long long)tf->error,
                (cr2 >= (USER_STACK_TOP - USER_STACK_SIZE) &&
                 cr2 < (USER_STACK_TOP - USER_STACK_SIZE + PAGE_SIZE))
                    ? "stack overflow" : "killing thread");
        sched_exit_thread(STATUS_ACCESS_VIOLATION);
        return;
    }

    kprintf("trap %llu (%s) err=%llx rip=%llx cs=%llx cr2=%llx\n",
            (unsigned long long)vector, exc_name(vector),
            (unsigned long long)tf->error,
            (unsigned long long)tf->rip,
            (unsigned long long)tf->cs,
            (unsigned long long)cr2);

    if (from_user(tf) && vector != 8 && vector != 2 && vector != 18) {
        kprintf("user %s — killing thread %s\n",
                exc_name(vector),
                ke_current() ? ke_current()->name : "?");
        sched_exit_thread(vector == 14 ? STATUS_ACCESS_VIOLATION :
                          vector == 6 ? STATUS_ILLEGAL_INSTRUCTION :
                          STATUS_UNSUCCESSFUL);
        return;
    }

    u64 gprs[20] = {
        tf->rax, tf->rbx, tf->rcx, tf->rdx, tf->rsi, tf->rdi, tf->rbp, tf->rsp,
        tf->r8,  tf->r9,  tf->r10, tf->r11, tf->r12, tf->r13, tf->r14, tf->r15,
        tf->rip, tf->rflags, cr2, tf->cs
    };
    panic_with_regs(exc_name(vector), gprs);
}

void cpu_enable_smap_smep(void)
{
    u32 a, b, c, d;
    a = 7; c = 0;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(a), "c"(c));
    u64 cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    if (b & (1u << 7)) cr4 |= (1ULL << 20); /* SMEP */
    if (b & (1u << 20)) cr4 |= (1ULL << 21); /* SMAP */
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
    kprintf("cpu: CR4=%llx SMEP=%d SMAP=%d\n",
            (unsigned long long)cr4,
            (int)!!(cr4 & (1ULL << 20)),
            (int)!!(cr4 & (1ULL << 21)));
}
