#include <jasos/syscall.h>
#include <jasos/string.h>
#include <jasos/config.h>
#include <stdarg.h>

/*
 * User printf. Opens /dev/console once and writes through NtWriteFile.
 * Linked only into ET_EXEC images, never the kernel.
 */
static handle_t con_out(void)
{
    static handle_t h;
    if (!h) {
        if (!NT_SUCCESS(NtCreateFile(&h, FILE_WRITE_DATA, "/dev/console",
                                     FILE_OPEN, FILE_NON_DIRECTORY_FILE)))
            h = 0;
    }
    return h;
}

static void uemit_buf(const char *s, u64 n)
{
    handle_t h = con_out();
    if (!h || !s || !n) return;
    u64 put = 0;
    NtWriteFile(h, s, n, 0, &put);
}

static void uemit(char c)
{
    uemit_buf(&c, 1);
}

static void uemit_str(const char *s)
{
    if (!s) s = "(null)";
    u64 n = 0;
    while (s[n]) n++;
    uemit_buf(s, n);
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = 0;
    for (; fmt && *fmt; fmt++) {
        if (*fmt != '%') { uemit(*fmt); n++; continue; }
        fmt++;
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            uemit_str(s);
            while (*s) { n++; s++; }
            break;
        }
        case 'c': uemit((char)va_arg(ap, int)); n++; break;
        case 'd': {
            int v = va_arg(ap, int);
            char b[16]; int i = 0;
            if (v < 0) { uemit('-'); n++; v = -v; }
            if (v == 0) b[i++] = '0';
            while (v && i < 16) { b[i++] = (char)('0' + (v % 10)); v /= 10; }
            while (i--) { uemit(b[i]); n++; }
            break;
        }
        case 'x': {
            unsigned v = va_arg(ap, unsigned);
            char b[16]; int i = 0;
            if (v == 0) b[i++] = '0';
            while (v && i < 16) {
                unsigned d = v & 15;
                b[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
                v >>= 4;
            }
            while (i--) { uemit(b[i]); n++; }
            break;
        }
        case '%': uemit('%'); n++; break;
        default: if (*fmt) { uemit(*fmt); n++; } break;
        }
    }
    va_end(ap);
    return n;
}