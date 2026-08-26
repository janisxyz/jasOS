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

extern void isr_stub_0(void);
extern void isr_stub_8(void);
extern void isr_stub_13(void);
extern void isr_stub_14(void);
extern void isr_stub_32(void);
extern void isr_stub_generic(void);

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
    idt_set(0,  isr_stub_0,  0);
    idt_set(8,  isr_stub_8,  1); /* DF → IST1 */
    idt_set(13, isr_stub_13, 0);
    idt_set(14, isr_stub_14, 0);
    idt_set(32, isr_stub_32, 0); /* PIT */
    idtr.len = sizeof(idt) - 1;
    idtr.ptr = (u64)idt;
    __asm__ volatile("lidt %0" :: "m"(idtr));
    kprintf("idt: 256 gates, IST1 on DF, IRQ0 at 32\n");
}

void isr_dispatch(u64 vector)
{
    if (vector == 32) {
        ke_on_tick();
        __asm__ volatile("outb %0, %1" :: "a"((u8)0x20), "Nd"((u16)0x20));
        return;
    }
    const char *n = "exception";
    if (vector == 0) n = "#DE divide";
    if (vector == 8) n = "#DF double fault";
    if (vector == 13) n = "#GP general protection";
    if (vector == 14) n = "#PF page fault";
    u64 cr2 = 0;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    kprintf("vector %llu cr2=%llx\n", (unsigned long long)vector, (unsigned long long)cr2);
    panic("%s", n);
}
