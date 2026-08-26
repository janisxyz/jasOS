#include <jasos/kprintf.h>
#include <jasos/config.h>

#ifndef JASOS_HOST

ALWAYS_INLINE void outb(u16 port, u8 v)
{
    __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(port));
}

ALWAYS_INLINE u8 inb(u16 port)
{
    u8 v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

#define COM SERIAL_COM1

extern int kbd_getchar(void);

void serial_init(void)
{
    outb(COM + 1, 0x00);    /* disable IRQ */
    outb(COM + 3, 0x80);    /* DLAB */
    outb(COM + 0, 0x01);    /* 115200 */
    outb(COM + 1, 0x00);
    outb(COM + 3, 0x03);    /* 8N1 */
    outb(COM + 2, 0xC7);    /* FIFO */
    outb(COM + 4, 0x0B);    /* RTS/DSR */
}

static void serial_wait_tx(void)
{
    u32 spins = 0;
    while ((inb(COM + 5) & 0x20) == 0) {
        if (++spins > 1000000) break; /* hardware ate the port; do not hang panic */
    }
}

void serial_putchar(char c)
{
    if (c == '\n') {
        serial_wait_tx();
        outb(COM, '\r');
    }
    serial_wait_tx();
    outb(COM, (u8)c);
}

void serial_write(const char *s, usize n)
{
    while (n--) serial_putchar(*s++);
}

int serial_poll_char(void)
{
    if (inb(COM + 5) & 0x01) return (int)inb(COM);
    return kbd_getchar();
}

void console_emit(char c)
{
    serial_putchar(c);
    /* VGA text at phys 0xB8000. Identity during early boot, HHDM after vmm. */
    static u32 col, row;
    static int scrolled;
    u16 *vga_id = (u16 *)(uintptr_t)0xB8000;
    u16 *vga_hh = (u16 *)(uintptr_t)(HHDM_BASE + 0xB8000);
    if (c == '\n') {
        col = 0;
        if (row < 24) row++;
        else scrolled = 1;
        return;
    }
    if (c == '\r') { col = 0; return; }
    if (c == '\t') {
        col = (col + 8) & ~7u;
        if (col >= 80) { col = 0; if (row < 24) row++; }
        return;
    }
    if (row >= 25 || col >= 80) return;
    u16 cell = (u16)(u8)c | 0x0700;
    vga_id[row * 80 + col] = cell;
    vga_hh[row * 80 + col] = cell;
    (void)scrolled;
    col++;
    if (col >= 80) { col = 0; if (row < 24) row++; }
}

#else /* JASOS_HOST */

void serial_init(void) {}

void serial_putchar(char c)
{
    fputc(c, stdout);
    if (c == '\n') fflush(stdout);
}

void serial_write(const char *s, usize n)
{
    fwrite(s, 1, n, stdout);
    fflush(stdout);
}

int serial_poll_char(void)
{
    int c = fgetc(stdin);
    return c;
}

void console_emit(char c)
{
    fputc(c, stdout);
    if (c == '\n') fflush(stdout);
}

#endif
