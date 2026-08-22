#ifndef STDIO_H
#define STDIO_H
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

int printf(const char *fmt, ...)  __attribute__((format(printf, 1, 2)));
int vprintf(const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...)
        __attribute__((format(printf, 3, 4)));
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int puts(const char *s);
int putchar(int c);

#endif
