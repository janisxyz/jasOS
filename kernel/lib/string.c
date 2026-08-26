#include <jasos/string.h>

#ifndef JASOS_HOST

void *memcpy(void *dst, const void *src, usize n)
{
    u8 *d = dst;
    const u8 *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, usize n)
{
    u8 *d = dst;
    const u8 *s = src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int c, usize n)
{
    u8 *d = dst;
    while (n--) *d++ = (u8)c;
    return dst;
}

int memcmp(const void *a, const void *b, usize n)
{
    const u8 *x = a, *y = b;
    while (n--) {
        if (*x != *y) return (int)*x - (int)*y;
        x++;
        y++;
    }
    return 0;
}

usize strlen(const char *s)
{
    usize n = 0;
    if (!s) return 0;
    while (*s++) n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    if (!a) a = "";
    if (!b) b = "";
    while (*a && *a == *b) { a++; b++; }
    return (int)(u8)*a - (int)(u8)*b;
}

int strncmp(const char *a, const char *b, usize n)
{
    if (!a) a = "";
    if (!b) b = "";
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (n == 0) return 0;
    return (int)(u8)*a - (int)(u8)*b;
}

char *strchr(const char *s, int c)
{
    if (!s) return NULL;
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return c == 0 ? (char *)s : NULL;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    if (!s) return NULL;
    do {
        if (*s == (char)c) last = s;
    } while (*s++);
    return (char *)last;
}

#endif /* !JASOS_HOST */

usize strlcpy(char *dst, const char *src, usize cap)
{
    usize n = 0;
    if (!dst || cap == 0) return src ? strlen(src) : 0;
    if (!src) { dst[0] = 0; return 0; }
    while (src[n] && n + 1 < cap) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = 0;
    while (src[n]) n++;
    return n;
}

usize strlcat(char *dst, const char *src, usize cap)
{
    usize d = dst ? strlen(dst) : 0;
    if (!dst || cap == 0) return d + (src ? strlen(src) : 0);
    return d + strlcpy(dst + d, src ? src : "", cap > d ? cap - d : 0);
}
