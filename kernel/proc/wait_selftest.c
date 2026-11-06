/* Boot-time tests for the process tree (spawn/wait/orphans, Day 21) and the console
 * TTY (Day 22). The ring-3 side of the checks lives in user/bin/spawntest.c and
 * user/bin/ttytest.c. */
#include "proc.h"
#include "pmm.h"
#include "sched.h"
#include "tty.h"
#include "timer.h"
#include "kernel.h"
#include "kprintf.h"

static int run_prog(const char *path) {
    int err = 0;
    const char *argv[] = { path, NULL };
    struct process *p = proc_spawn_elf(path, argv, &err);
    if (!p) panic("spawning %s failed (%d)", path, err);
    return proc_wait(p, NULL);
}

/* Orphans are freed asynchronously by the reaper thread. */
static void wait_live(unsigned want) {
    uint64_t end = timer_ms() + 3000;
    while (proc_live_count() != want) {
        if (timer_ms() > end) panic("proc: %u processes live, expected %u", proc_live_count(), want);
        thread_sleep_ms(10);
    }
}

void test_wait(void) {
    unsigned live0 = proc_live_count();
    int code = run_prog("/bin/spawntest");      /* warm-up: heap reaches its working size */
    if (code) panic("spawntest failed check %d", code);
    wait_live(live0);
    uint64_t frames0 = pmm_free_frames();
    code = run_prog("/bin/spawntest");
    if (code) panic("spawntest (2nd run) failed check %d", code);
    if (proc_live_count() == live0) panic("proc: orphans gone too early");
    wait_live(live0);
    if (pmm_free_frames() != frames0)
        panic("proc: frames leaked: %lu -> %lu", frames0, pmm_free_frames());
    kprintf("[TEST] process tree ok: orphans reaped, %u processes live, no frames leaked\n",
            proc_live_count());
}

void test_tty(void) {
    struct tty_stats s0, s1;
    tty_get_stats(&s0);
    int code = run_prog("/bin/ttytest");
    if (code) panic("ttytest failed check %d", code);
    tty_get_stats(&s1);
    uint64_t lines = s1.lines - s0.lines, erased = s1.erased - s0.erased;
    if (lines != 2 || erased != 3 || s1.eofs - s0.eofs != 1)
        panic("tty: %lu lines %lu erased %lu eofs", lines, erased, s1.eofs - s0.eofs);
    kprintf("[TEST] tty ok: %lu lines, %lu chars erased, 1 eof\n", lines, erased);
}

/* Day 23/24: the shell runs a script from the initrd, then (in images with keyboard
 * input) an interactive session typed by the harness. */
void test_shell(bool interactive) {
    int err = 0;
    const char *argv[] = { "tsh", "/etc/shtest.sh", NULL };
    struct process *p = proc_spawn_elf("/bin/tsh", argv, &err);
    if (!p) panic("spawning tsh failed (%d)", err);
    int code = proc_wait(p, NULL);
    if (code != 5) panic("tsh script: exit %d, wanted 5", code);
    kprintf("[TEST] shell script ok: exit %d\n", code);
    if (!interactive) return;
    const char *iargv[] = { "tsh", NULL };
    p = proc_spawn_elf("/bin/tsh", iargv, &err);
    if (!p) panic("spawning tsh failed (%d)", err);
    code = proc_wait(p, NULL);
    if (code != 3) panic("tsh interactive: exit %d, wanted 3", code);
    kprintf("[TEST] shell ok: interactive session, exit %d\n", code);
}
