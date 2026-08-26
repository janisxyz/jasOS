#pragma once

#include <jasos/types.h>

#ifndef JASOS_HOST
void  *memcpy(void *dst, const void *src, usize n);
void  *memmove(void *dst, const void *src, usize n);
void  *memset(void *dst, int c, usize n);
int    memcmp(const void *a, const void *b, usize n);
usize  strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, usize n);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
#endif

usize  strlcpy(char *dst, const char *src, usize cap);
usize  strlcat(char *dst, const char *src, usize cap);
