/* TernOS user library: strings and a small printf (%d %i %u %x %X %p %s %c %%, with
 * l/ll/z length, '-', '0' and width). */
#include "tern.h"

void *memset(void *d, int c, size_t n) {
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}
void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *a = d;
    const unsigned char *b = s;
    while (n--) *a++ = *b++;
    return d;
}
void *memmove(void *d, const void *s, size_t n) {
    unsigned char *a = d;
    const unsigned char *b = s;
    if (a < b) while (n--) *a++ = *b++;
    else while (n--) a[n] = b[n];
    return d;
}
int memcmp(const void *x, const void *y, size_t n) {
    const unsigned char *a = x, *b = y;
    for (; n; n--, a++, b++) if (*a != *b) return *a - *b;
    return 0;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        if (!*a) return 0;
    }
    return 0;
}
char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return NULL;
    }
}

struct out { char *buf; size_t size, len; };
static void put(struct out *o, char c) {
    if (o->len + 1 < o->size) o->buf[o->len] = c;
    o->len++;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    struct out o = { buf, size, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') { put(&o, *fmt); continue; }
        fmt++;
        bool left = false, zero = false;
        for (;; fmt++) {
            if (*fmt == '-') left = true;
            else if (*fmt == '0') zero = true;
            else break;
        }
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        int lng = 0;
        while (*fmt == 'l' || *fmt == 'z') { lng++; fmt++; }
        char tmp[24];
        const char *s = tmp;
        int n = 0;
        bool neg = false;
        switch (*fmt) {
        case 'd': case 'i': {
            long long v = lng ? va_arg(ap, long long) : va_arg(ap, int);
            unsigned long long u = v < 0 ? 0ull - (unsigned long long)v : (unsigned long long)v;
            neg = v < 0;
            do { tmp[sizeof tmp - 1 - n++] = (char)('0' + u % 10); u /= 10; } while (u);
            s = tmp + sizeof tmp - n;
            break;
        }
        case 'u': case 'x': case 'X': case 'p': {
            unsigned long long u;
            if (*fmt == 'p') { u = (uintptr_t)va_arg(ap, void *); put(&o, '0'); put(&o, 'x'); }
            else u = lng ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned);
            unsigned base = *fmt == 'u' ? 10 : 16;
            const char *dig = *fmt == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            do { tmp[sizeof tmp - 1 - n++] = dig[u % base]; u /= base; } while (u);
            s = tmp + sizeof tmp - n;
            break;
        }
        case 's':
            s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            n = (int)strlen(s);
            break;
        case 'c':
            tmp[0] = (char)va_arg(ap, int);
            n = 1;
            break;
        case '%':
            put(&o, '%');
            continue;
        case 0:
            fmt--;
            continue;
        default:
            put(&o, '%'); put(&o, *fmt);
            continue;
        }
        int len = n + (neg ? 1 : 0);
        if (neg && zero) put(&o, '-');
        if (!left) for (int i = len; i < width; i++) put(&o, zero ? '0' : ' ');
        if (neg && !zero) put(&o, '-');
        for (int i = 0; i < n; i++) put(&o, s[i]);
        if (left) for (int i = len; i < width; i++) put(&o, ' ');
    }
    if (size) buf[o.len < size ? o.len : size - 1] = 0;
    return (int)o.len;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

static int vdprintf(int fd, const char *fmt, va_list ap) {
    char buf[512];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    write(fd, buf, n < (int)sizeof buf ? (size_t)n : sizeof buf - 1);
    return n;
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vdprintf(1, fmt, ap);
    va_end(ap);
    return n;
}

int dprintf(int fd, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vdprintf(fd, fmt, ap);
    va_end(ap);
    return n;
}

int puts(const char *s) {
    size_t n = strlen(s);
    write(1, s, n);
    write(1, "\n", 1);
    return (int)n + 1;
}
