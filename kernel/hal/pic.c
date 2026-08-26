#include <jasos/ke.h>
#include <jasos/kprintf.h>

/*
 * 8259 PIC. v1 clock and keyboard live here.
 *
 * LAPIC is the next HAL, not this one. Do not print "LAPIC" from
 * this file. QEMU -kernel brings up a usable 8259/PIT without MADT.
 * Contract for the replacement: hal_timer_init(hz) still calls
 * ke_on_tick(); ke/sched.c does not change.
 */

static inline void outb(u16 p, u8 v) { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(p)); }
static inline u8  inb(u16 p) { u8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }

void pic_remap(u8 off1, u8 off2)
{
    u8 a1 = inb(0x21), a2 = inb(0xA1);
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, off1); outb(0xA1, off2);
    outb(0x21, 0x04); outb(0xA1, 0x02);
    outb(0x21, 0x01); outb(0xA1, 0x01);
    outb(0x21, 0xFF); outb(0xA1, 0xFF);
    (void)a1; (void)a2;
    kprintf("pic: 8259 remapped master %u slave %u, all masked\n", off1, off2);
}

void pic_unmask(u8 irq)
{
    u16 port = irq < 8 ? 0x21 : 0xA1;
    u8 bit = irq < 8 ? irq : (u8)(irq - 8);
    u8 m = 0;
    __asm__ volatile("inb %1, %0" : "=a"(m) : "Nd"(port));
    m &= (u8)~(1u << bit);
    outb(port, m);
}

void pic_eoi(u8 irq)
{
    if (irq >= 8) outb(0xA0, 0x20);
    outb(0x20, 0x20);
}
