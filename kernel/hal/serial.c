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
    return -1;
}

void console_emit(char c)
{
    serial_putchar(c);
    /* VGA text fallback so a machine without COM still shows life. */
    static u16 *vga = (u16 *)(KERNEL_VMA + 0xB8000);
    static u32 col, row;
    /* Before higher-half we may still be identity-mapped; try both. */
    u16 *v = vga;
    if (c == '\n') {
        col = 0;
        if (row < 24) row++;
        return;
    }
    if (c == '\r') { col = 0; return; }
    if (row >= 25 || col >= 80) return;
    /* Physical 0xB8000 is in the HHDM after vmm_init; during early boot
       identity map makes 0xB8000 work. Write both. */
    ((u16 *)0xB8000)[row * 80 + col] = (u16)c | 0x0700;
    (void)v;
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
