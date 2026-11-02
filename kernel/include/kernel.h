#ifndef TERN_KERNEL_H
#define TERN_KERNEL_H
#include <stdbool.h>

#define TERNOS_NAME    "TernOS"
#define TERNOS_VERSION "0.1.0"

/* QEMU isa-debug-exit (iobase 0xf4): exit status = (code << 1) | 1. */
#define QEMU_EXIT_SUCCESS 0x10   /* -> host status 33 */
#define QEMU_EXIT_FAILURE 0x11   /* -> host status 35 */
void qemu_exit(int code);

__attribute__((noreturn, format(printf, 1, 2))) void panic(const char *fmt, ...);

#define kassert(cond) \
    do { if (!(cond)) panic("assertion failed: %s (%s:%d)", #cond, __FILE__, __LINE__); } while (0)

extern bool g_test_mode;

#endif
