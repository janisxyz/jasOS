#include <jasos/ke.h>
#include <jasos/kprintf.h>
#include <jasos/config.h>

/*
 * PS/2 keyboard, scancode set 1, IRQ1 (vector 33).
 *
 * Why this will fail in production:
 *  - No make/break pairing for modifiers beyond shift.
 *  - No USB HID. A machine with USB-only keyboard is silent.
 *  - We also have COM1; sh prefers serial. Keyboard fills a typeahead
 *    ring that serial_poll_char will drain if COM is empty.
 */

static inline void outb(u16 p, u8 v) { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(p)); }
static inline u8  inb(u16 p) { u8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }

#define KBD_BUF 32
static char g_kbd[KBD_BUF];
static u32  g_kbd_r, g_kbd_w;
static int  g_shift;

static const char map[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n', 0,
    'a','s','d','f','g','h','j','k','l',';','\'','`', 0, '\\',
    'z','x','c','v','b','n','m',',','.','/', 0, '*', 0, ' ',
};

static const char map_shift[128] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n', 0,
    'A','S','D','F','G','H','J','K','L',':','"','~', 0, '|',
    'Z','X','C','V','B','N','M','<','>','?', 0, '*', 0, ' ',
};

void kbd_isr(void)
{
    u8 sc = inb(0x60);
    if (sc == 0x2A || sc == 0x36) { g_shift = 1; goto eoi; }
    if (sc == 0xAA || sc == 0xB6) { g_shift = 0; goto eoi; }
    if (sc & 0x80) goto eoi;
    char c = g_shift ? map_shift[sc] : map[sc];
    if (c) {
        u32 n = (g_kbd_w + 1) % KBD_BUF;
        if (n != g_kbd_r) {
            g_kbd[g_kbd_w] = c;
            g_kbd_w = n;
        }
    }
eoi:
    __asm__ volatile("outb %0, %1" :: "a"((u8)0x20), "Nd"((u16)0x20));
}

int kbd_getchar(void)
{
    if (g_kbd_r == g_kbd_w) return -1;
    char c = g_kbd[g_kbd_r];
    g_kbd_r = (g_kbd_r + 1) % KBD_BUF;
    return (int)(u8)c;
}

void kbd_init(void)
{
    /* drain */
    while (inb(0x64) & 1) (void)inb(0x60);
    pic_unmask(1);
    kprintf("kbd: ps/2 irq1, typeahead %u\n", KBD_BUF);
}
