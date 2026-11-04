/* In-kernel unit-test runner (see ktest.h). */
#include "ktest.h"
#include "kprintf.h"

extern const struct ktest __ktests_start[], __ktests_end[];
static unsigned failures;

void ktest_fail(const char *expr, const char *file, int line) {
    failures++;
    kprintf("    expect failed: %s (%s:%d)\n", expr, file, line);
}

int ktest_run_all(void) {
    unsigned total = (unsigned)(__ktests_end - __ktests_start), passed = 0;
    kprintf("selftest: %u unit tests\n", total);
    for (const struct ktest *t = __ktests_start; t < __ktests_end; t++) {
        unsigned before = failures;
        t->fn();
        bool ok = failures == before;
        if (ok) passed++;
        kprintf("  [%s] %s (%s)\n", ok ? " ok " : "FAIL", t->name, t->file);
    }
    kprintf("[TEST] selftest %u/%u passed\n", passed, total);
    return (int)(total - passed);
}
