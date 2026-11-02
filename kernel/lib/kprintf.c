/* Minimal freestanding printf family. */
#include "kprintf.h"
#include "console.h"
#include <stdint.h>
#include <stdbool.h>

struct out { char *buf; size_t size, pos; };

static void emit(struct out *o, char c) {
    if (o->pos + 1 < o->size) o->buf[o->pos] = c;
    o->pos++;
}

static void emit_num(struct out *o, uint64_t v, unsigned base, bool upper, bool neg,
                     int width, bool zero, bool left) {
    char tmp[24];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    do { tmp[n++] = digits[v % base]; v /= base; } while (v);
    int len = n + (neg ? 1 : 0);
    int pad = width > len ? width - len : 0;
    if (!left && !zero) while (pad-- > 0) emit(o, ' ');
    if (neg) emit(o, '-');
    if (!left && zero) while (pad-- > 0) emit(o, '0');
    while (n) emit(o, tmp[--n]);
    if (left) while (pad-- > 0) emit(o, ' ');
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    struct out o = { buf, size, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') { emit(&o, *fmt); continue; }
        fmt++;
        bool zero = false, left = false;
        for (;; fmt++) {
            if (*fmt == '0') zero = true;
            else if (*fmt == '-') left = true;
            else break;
        }
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        int lng = 0;
        while (*fmt == 'l') { lng++; fmt++; }
        if (*fmt == 'z') { lng = 2; fmt++; }
        switch (*fmt) {
        case 'c': emit(&o, (char)va_arg(ap, int)); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = 0;
            while (s[len]) len++;
            int pad = width > len ? width - len : 0;
            if (!left) while (pad-- > 0) emit(&o, ' ');
            while (*s) emit(&o, *s++);
            if (left) while (pad-- > 0) emit(&o, ' ');
            break;
        }
        case 'd': case 'i': {
            int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
            bool neg = v < 0;
            uint64_t u = neg ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
            emit_num(&o, u, 10, false, neg, width, zero, left);
            break;
        }
        case 'u': case 'x': case 'X': {
            uint64_t v = lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned);
            unsigned base = *fmt == 'u' ? 10 : 16;
            emit_num(&o, v, base, *fmt == 'X', false, width, zero, left);
            break;
        }
        case 'p':
            emit(&o, '0'); emit(&o, 'x');
            emit_num(&o, (uintptr_t)va_arg(ap, void *), 16, false, false, 16, true, false);
            break;
        case '%': emit(&o, '%'); break;
        case '\0': fmt--; break;
        default: emit(&o, '%'); emit(&o, *fmt); break;
        }
    }
    if (o.size) o.buf[o.pos < o.size ? o.pos : o.size - 1] = '\0';
    return (int)o.pos;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

int kprintf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    console_write(buf);
    return n;
}
