#pragma once

#include <jasos/types.h>

void kprintf(const char *fmt, ...);
void kvprintf(const char *fmt, va_list ap);
void kputs(const char *s);
void kputchar(char c);

/* Panic path: never takes a lock, never allocates, never lies. */
void NORETURN panic(const char *fmt, ...);
void NORETURN panic_with_regs(const char *why, const u64 *gprs);

void serial_init(void);
void serial_write(const char *s, usize n);
void serial_putchar(char c);
int  serial_poll_char(void); /* -1 if empty */

/* Host and kernel both call this; host writes stdout. */
void console_emit(char c);
