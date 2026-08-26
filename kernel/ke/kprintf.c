#include <jasos/kprintf.h>
#include <jasos/config.h>
#include <jasos/string.h>
#include <jasos/ke.h>

/*
 * Lock-free on panic. A nested kprintf during panic must not take the
 * serial spinlock — that is how Linux and NT both lie to you with a hang
 * instead of a dump.
 */

static spinlock_t serial_print_lock = SPINLOCK_INIT("kprintf", LOCK_RANK_SERIAL);
static volatile u32 kprintf_depth;

static void emit(char c)
{
    console_emit(c);
}

static void emit_n(const char *s, usize n)
{
    while (n--) emit(*s++);
}

static void emit_str(const char *s)
{
    if (!s) s = "(null)";
    while (*s) emit(*s++);
}

static void emit_u(u64 v, u32 base, bool upper, int width, char pad)
{
    char buf[32];
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0;
    if (base < 2 || base > 16) base = 10;
    if (v == 0) buf[i++] = '0';
    while (v && i < 32) {
        buf[i++] = dig[v % base];
        v /= base;
    }
    while (i < width && i < 32) buf[i++] = pad;
    while (i--) emit(buf[i]);
}

static void emit_i(i64 v, int width)
{
    if (v < 0) {
        emit('-');
        emit_u((u64)(-v), 10, false, width > 0 ? width - 1 : 0, ' ');
    } else {
        emit_u((u64)v, 10, false, width, ' ');
    }
}

void kvprintf(const char *fmt, va_list ap)
{
    if (!fmt) return;
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            emit(*fmt);
            continue;
        }
        fmt++;
        int width = 0;
        char pad = ' ';
        while (*fmt == '-' || *fmt == '+' || *fmt == ' ') fmt++;
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        int longness = 0;
        while (*fmt == 'l') { longness++; fmt++; }
        if (*fmt == 'z') { longness = 2; fmt++; }
        switch (*fmt) {
        case '%': emit('%'); break;
        case 'c': emit((char)va_arg(ap, int)); break;
        case 's': emit_str(va_arg(ap, const char *)); break;
        case 'd':
        case 'i':
            if (longness >= 2) emit_i(va_arg(ap, i64), width);
            else emit_i(va_arg(ap, int), width);
            break;
        case 'u':
            if (longness >= 2) emit_u(va_arg(ap, u64), 10, false, width, pad);
            else emit_u(va_arg(ap, u32), 10, false, width, pad);
            break;
        case 'x':
            if (longness >= 2) emit_u(va_arg(ap, u64), 16, false, width, pad);
            else emit_u(va_arg(ap, u32), 16, false, width, pad);
            break;
        case 'X':
            if (longness >= 2) emit_u(va_arg(ap, u64), 16, true, width, pad);
            else emit_u(va_arg(ap, u32), 16, true, width, pad);
            break;
        case 'p':
            emit_str("0x");
            emit_u((u64)va_arg(ap, void *), 16, false, 16, '0');
            break;
        default:
            emit('%');
            if (*fmt) emit(*fmt);
            break;
        }
    }
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    u32 d = atomic_inc32(&kprintf_depth);
    bool take = (d == 1) && !g_panic_in_progress;
    if (take) spin_lock(&serial_print_lock);
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    if (take) spin_unlock(&serial_print_lock);
    atomic_dec32(&kprintf_depth);
}

void kputs(const char *s)
{
    kprintf("%s", s);
}

void kputchar(char c)
{
    u32 d = atomic_inc32(&kprintf_depth);
    bool take = (d == 1) && !g_panic_in_progress;
    if (take) spin_lock(&serial_print_lock);
    emit(c);
    if (take) spin_unlock(&serial_print_lock);
    atomic_dec32(&kprintf_depth);
}


