#ifndef TERN_KPRINTF_H
#define TERN_KPRINTF_H
#include <stdarg.h>
#include <stddef.h>

/* Supported: %c %s %d %i %u %x %X %p %%, flags '0' '-', width, length 'l' 'll' 'z'. */
int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int ksnprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
