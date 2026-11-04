/* Unit tests: string functions, kprintf formatting, symbols and backtraces. */
#include <stdint.h>
#include "ktest.h"
#include "string.h"
#include "kprintf.h"
#include "ksyms.h"

KTEST(string_mem) {
    char a[16], b[16];
    memset(a, 'x', sizeof a);
    KEXPECT(a[0] == 'x' && a[15] == 'x');
    memcpy(b, "abcdefghijklmno", 16);
    KEXPECT(memcmp(b, "abcdefghijklmno", 16) == 0);
    KEXPECT(memcmp("abc", "abd", 3) < 0 && memcmp("abd", "abc", 3) > 0);
    memmove(b + 2, b, 10);                       /* overlapping, forwards */
    KEXPECT(memcmp(b, "ababcdefghijmno", 15) == 0);
    memmove(b, b + 2, 10);                       /* overlapping, backwards */
    KEXPECT(memcmp(b, "abcdefghijijmno", 15) == 0);
}

KTEST(string_str) {
    KEXPECT(strlen("") == 0 && strlen("tern") == 4);
    KEXPECT(strcmp("a", "a") == 0 && strcmp("a", "b") < 0 && strcmp("b", "a") > 0);
    KEXPECT(strcmp("ab", "a") > 0);
    KEXPECT(strncmp("abcx", "abcy", 3) == 0 && strncmp("abcx", "abcy", 4) < 0);
    const char *h = "test kbd noapic";
    KEXPECT(strstr(h, "noapic") == h + 9);
    KEXPECT(strstr(h, "") == h);
    KEXPECT(strstr(h, "apicx") == NULL);
}

KTEST(kprintf_format) {
    char b[64];
    ksnprintf(b, sizeof b, "%d|%5u|%-3s|%x|%08X", -42, 7u, "a", 0xbeefu, 0x1Fu);
    KEXPECT(strcmp(b, "-42|    7|a  |beef|0000001F") == 0);
    ksnprintf(b, sizeof b, "%lu %lx", (unsigned long)18446744073709551615ull,
              (unsigned long)0x123456789abcull);
    KEXPECT(strcmp(b, "18446744073709551615 123456789abc") == 0);
    ksnprintf(b, sizeof b, "%c%s%%", 'T', "ern");
    KEXPECT(strcmp(b, "Tern%") == 0);
    int n = ksnprintf(b, 4, "abcdef");
    KEXPECT(n == 6 && strcmp(b, "abc") == 0);
    ksnprintf(b, sizeof b, "%d %d", 0, -2147483647 - 1);
    KEXPECT(strcmp(b, "0 -2147483648") == 0);
}

KTEST(ksyms_lookup) {
    KEXPECT(ksyms_count > 50);
    for (unsigned i = 1; i < ksyms_count; i++)
        if (ksyms[i - 1].addr >= ksyms[i].addr) { KEXPECT(!"symbol table sorted"); break; }
    uint64_t off = 99;
    const char *name = ksym_lookup((uint64_t)&ksym_lookup, &off);
    KEXPECT(name && strcmp(name, "ksym_lookup") == 0 && off == 0);
    name = ksym_lookup((uint64_t)&ksym_lookup + 5, &off);
    KEXPECT(name && strcmp(name, "ksym_lookup") == 0 && off == 5);
    KEXPECT(ksym_addr("kmain") != 0 && ksym_addr("no_such_symbol") == 0);
    KEXPECT(ksym_lookup((uint64_t)&ksyms_count, &off) == NULL);    /* rodata, not code */
    KEXPECT(ksym_lookup(0, &off) == NULL);
}

/* A known call chain: bt_outer -> bt_middle -> bt_inner, which collects the backtrace.
 * noinline + an empty asm after each call keep the frames real. */
static int bt_n;
static uint64_t bt_frames[BT_MAX_FRAMES];
__attribute__((noinline)) static void bt_inner(void) {
    bt_n = backtrace_collect((uint64_t)__builtin_frame_address(0), bt_frames, BT_MAX_FRAMES);
    __asm__ volatile("");
}
__attribute__((noinline)) static void bt_middle(void) { bt_inner(); __asm__ volatile(""); }
__attribute__((noinline)) static void bt_outer(void) { bt_middle(); __asm__ volatile(""); }

static bool frame_is(int i, const char *fn) {
    uint64_t off;
    const char *n = i < bt_n ? ksym_lookup(bt_frames[i] - 1, &off) : NULL;
    return n && strcmp(n, fn) == 0;
}

KTEST(backtrace_chain) {
    bt_outer();
    /* frame 0 is bt_inner's return address into bt_middle, etc. */
    KEXPECT(bt_n >= 4);
    KEXPECT(frame_is(0, "bt_middle"));
    KEXPECT(frame_is(1, "bt_outer"));
    KEXPECT(frame_is(2, "ktest_fn_backtrace_chain"));
    KEXPECT(frame_is(3, "ktest_run_all"));
    /* garbage frame pointers stop the walk instead of faulting */
    uint64_t f[4];
    KEXPECT(backtrace_collect(0x7FFF00000000ull, f, 4) == 0);
    KEXPECT(backtrace_collect(0x8000000000000000ull, f, 4) == 0);
    KEXPECT(backtrace_collect(0x1234567, f, 4) == 0);
}
