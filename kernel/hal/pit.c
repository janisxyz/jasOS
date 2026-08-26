#include <jasos/ke.h>
#include <jasos/kprintf.h>
#include <jasos/config.h>

static inline void outb(u16 p, u8 v) { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(p)); }

void pit_init(u32 hz)
{
    if (hz == 0) hz = TIMER_HZ;
    u32 div = 1193182u / hz;
    outb(0x43, 0x36);
    outb(0x40, (u8)(div & 0xFF));
    outb(0x40, (u8)((div >> 8) & 0xFF));
    kprintf("pit: %u Hz (div %u) — preemption clock\n", hz, div);
}
