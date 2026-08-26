#include <jasos/ke.h>
#include <jasos/kprintf.h>
#include <jasos/string.h>

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
extern void kbd_isr(void);

static void idt_set(int v, void (*fn)(void), u8 ist)
{
    u64 a = (u64)fn;
    idt[v].off_low  = (u16)a;
    idt[v].sel      = 0x08;
    idt[v].ist      = ist;
    idt[v].type     = 0x8E;
    idt[v].off_mid  = (u16)(a >> 16);
    idt[v].off_high = (u32)(a >> 32);
    idt[v].zero     = 0;
}

void idt_init(void)
{
    memset(idt, 0, sizeof(idt));
    for (int i = 0; i < 256; i++) idt_set(i, isr_stub_generic, 0);
    for (int i = 0; i < 34; i++) {
        u8 ist = (i == 8) ? 1 : (i == 2) ? 2 : (i == 18) ? 3 : 0;
        idt_set(i, (void (*)(void))isr_stub_table[i], ist);
    }
    idtr.len = sizeof(idt) - 1;
    idtr.ptr = (u64)idt;
    __asm__ volatile("lidt %0" :: "m"(idtr));
    kprintf("idt: 256 gates, IST1 DF IST2 NMI IST3 MC, IRQ0=32 IRQ1=33\n");
}

void isr_dispatch(u64 vector)
{
    if (vector == 32) {
        ke_on_tick();
        __asm__ volatile("outb %0, %1" :: "a"((u8)0x20), "Nd"((u16)0x20));
        return;
    }
    if (vector == 33) {
        kbd_isr();
        return;
    }
    static const char *names[32] = {
        "#DE","#DB","NMI","#BP","#OF","#BR","#UD","#NM",
        "#DF","cso","#TS","#NP","#SS","#GP","#PF","spu",
        "#MF","#AC","#MC","#XM","#VE","#CP","22","23",
        "24","25","26","27","#HV","#VC","#SX","31"
    };
    u64 cr2 = 0;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    const char *n = (vector < 32) ? names[vector] : "irq/exc";
    kprintf("vector %llu (%s) cr2=%llx\n",
            (unsigned long long)vector, n, (unsigned long long)cr2);
    if (vector == 14 && ke_current() && ke_current()->process &&
        ke_current()->process->pid != 0) {
        kprintf("user #PF — killing thread %s\n", ke_current()->name);
        sched_exit_thread(STATUS_ACCESS_VIOLATION);
        return;
    }
    panic("%s", n);
}
