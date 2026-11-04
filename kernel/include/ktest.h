#ifndef TERN_KTEST_H
#define TERN_KTEST_H
/* In-kernel unit tests. Define a test anywhere in the kernel with
 *     KTEST(name) { KEXPECT(1 + 1 == 2); }
 * It is registered in the .ktests section and run by ktest_run_all() in test mode. */
#include <stdbool.h>
#include "kprintf.h"

struct ktest { const char *name; void (*fn)(void); const char *file; };

void ktest_fail(const char *expr, const char *file, int line);
/* Runs every registered test; returns the number of failed tests. */
int ktest_run_all(void);

#define KTEST(tname) \
    static void ktest_fn_##tname(void); \
    __attribute__((used, section(".ktests"), aligned(8))) \
    static const struct ktest ktest_desc_##tname = { #tname, ktest_fn_##tname, __FILE__ }; \
    static void ktest_fn_##tname(void)

#define KEXPECT(cond) \
    do { if (!(cond)) ktest_fail(#cond, __FILE__, __LINE__); } while (0)

#endif
