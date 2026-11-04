/* Symbol lookup and frame-pointer backtraces. */
#include <stdbool.h>
#include "ksyms.h"
#include "extable.h"
#include "kprintf.h"
#include "string.h"
#include "mm.h"

const char *ksym_lookup(uint64_t addr, uint64_t *offset) {
    if (addr < (uint64_t)__text_start || addr >= (uint64_t)__text_end || !ksyms_count)
        return NULL;
    unsigned lo = 0, hi = ksyms_count;          /* find the last entry with addr <= x */
    while (hi - lo > 1) {
        unsigned mid = (lo + hi) / 2;
        if (ksyms[mid].addr <= addr) lo = mid; else hi = mid;
    }
    if (ksyms[lo].addr > addr) return NULL;
    if (offset) *offset = addr - ksyms[lo].addr;
    return ksyms[lo].name;
}

uint64_t ksym_addr(const char *name) {
    for (unsigned i = 0; i < ksyms_count; i++)
        if (!strcmp(ksyms[i].name, name)) return ksyms[i].addr;
    return 0;
}

int backtrace_collect(uint64_t rbp, uint64_t *out, int max) {
    int n = 0;
    while (n < max && rbp && !(rbp & 7)) {
        uint64_t next, ret;
        if (probe_read64((const void *)rbp, &next) || probe_read64((const void *)(rbp + 8), &ret))
            break;
        if (!ret || ret < (uint64_t)__text_start || ret >= (uint64_t)__text_end) break;
        out[n++] = ret;
        if (next <= rbp) break;                 /* stacks grow down: callers are higher */
        rbp = next;
    }
    return n;
}

static void print_frame(int i, uint64_t addr, bool is_ret) {
    uint64_t off;
    /* A return address points after the call; look up addr-1 so a call that ends a
     * function (noreturn callee) is attributed to the right one. */
    const char *name = ksym_lookup(is_ret ? addr - 1 : addr, &off);
    if (name) kprintf("  #%d %016lx %s+0x%lx\n", i, addr, name, is_ret ? off + 1 : off);
    else kprintf("  #%d %016lx ?\n", i, addr);
}

void backtrace_print(uint64_t rip, uint64_t rbp) {
    uint64_t frames[BT_MAX_FRAMES];
    int n = backtrace_collect(rbp, frames, BT_MAX_FRAMES);
    int i = 0;
    kprintf("backtrace:\n");
    if (rip) print_frame(i++, rip, false);
    for (int k = 0; k < n; k++) print_frame(i++, frames[k], true);
}
