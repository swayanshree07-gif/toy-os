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
#include "vfs.h"
#include "heap.h"

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

/* Day 9 session: kill / interrupted waits / spawn fds / proclist (user/bin/killtest.c). */
void test_kill(void) {
    unsigned live0 = proc_live_count();
    wait_live(live0);
    int code = run_prog("/bin/killtest");
    if (code) panic("killtest failed check %d", code);
    wait_live(live0);                        /* the orphaned "child sleep 300" too */
}

/* Two user processes keep different values in xmm0-15 / x87 across context switches. */
void test_fpu(void) {
    struct sched_stats s0, s1;
    sched_get_stats(&s0);
    struct process *p[2];
    for (int i = 0; i < 2; i++) {
        int err = 0;
        const char *argv[] = { "fputest", i ? "2" : "1", NULL };
        p[i] = proc_spawn_elf("/bin/fputest", argv, &err);
        if (!p[i]) panic("spawning fputest failed (%d)", err);
    }
    struct process info[2];
    for (int i = 0; i < 2; i++) {
        int code = proc_wait(p[i], &info[i]);
        if (code) panic("fputest %d failed: exit %d", i + 1, code);
    }
    sched_get_stats(&s1);
    kprintf("[TEST] fpu ok: 2 processes kept xmm0-15/x87/double state over %lu+%lu switches, "
            "%lu fxsaves\n", info[0].switches, info[1].switches, s1.fpu_saves - s0.fpu_saves);
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

/* Day 10 session: pipes, process groups, rename, process limit (user/bin/pipetest.c),
 * then the system-call fuzzer with one seed, and twice with another: the last run must leave the
 * frame count, VFS node count and pipe count exactly where they were. */
static int run_argv(const char *const *argv) {
    int err = 0;
    struct process *p = proc_spawn_elf(argv[0], argv, &err);
    if (!p) panic("spawning %s failed (%d)", argv[0], err);
    return proc_wait(p, NULL);
}

void test_pipes(void) {
    unsigned live0 = proc_live_count();
    uint64_t t0 = timer_ms();
    int code = run_prog("/bin/pipetest");
    if (code) panic("pipetest failed check %d", code);
    wait_live(live0);
    kprintf("pipetest took %lu ms\n", timer_ms() - t0);
    const char *fz2[] = { "/bin/fuzz", "100000", "777", NULL };
    if ((code = run_argv(fz2))) panic("fuzz (seed 777) exit %d", code);
    const char *fz[] = { "/bin/fuzz", "100000", "12345", NULL };
    if ((code = run_argv(fz))) panic("fuzz (warm-up) exit %d", code);
    wait_live(live0);
    uint64_t frames0 = pmm_free_frames();
    struct vfs_stats v0, v1;
    struct pipe_stats p0, p1;
    vfs_get_stats(&v0);
    pipe_get_stats(&p0);
    if ((code = run_argv(fz))) panic("fuzz exit %d", code);
    wait_live(live0);
    vfs_get_stats(&v1);
    pipe_get_stats(&p1);
    struct heap_stats hs;
    heap_check(&hs);
    if (pmm_free_frames() != frames0)
        panic("fuzz: frames leaked: %lu -> %lu", frames0, pmm_free_frames());
    if (v1.nodes != v0.nodes || p1.live != p0.live || p1.live != 0)
        panic("fuzz: vfs nodes %lu -> %lu, pipes live %lu -> %lu", v0.nodes, v1.nodes,
              p0.live, p1.live);
    kprintf("[TEST] robustness ok: 3x100000 fuzzed syscalls, kernel heap consistent, no frames/"
            "nodes/pipes leaked, %lu pipe bytes moved so far\n", p1.bytes);
}

/* Day 11 session: user framebuffer access. /bin/demo --test checks the syscall errors,
 * fills the screen with a known pattern and exits while still owning the screen. Here
 * we check that the pattern really reached the framebuffer, that console output
 * meanwhile doesn't draw over it, and that the console repaints itself (including the
 * text printed while it was hidden) once the process is gone. */
#include "fb.h"
#include "console.h"
#include "fbdev.h"
#include "string.h"

static bool fb_pattern_ok(void) {
    static const uint32_t pts[][2] = { {0, 0}, {1, 0}, {5, 3}, {100, 37}, {317, 199} };
    for (unsigned i = 0; i < sizeof pts / sizeof pts[0]; i++) {
        uint32_t x = pts[i][0], y = pts[i][1], v = ((x * 7) ^ (y * 13)) & 0xffffff;
        if (fb_getpixel(x, y) != fb_rgb((uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v))
            return false;
    }
    uint32_t x = g_fb.width - 1, y = g_fb.height - 1, v = ((x * 7) ^ (y * 13)) & 0xffffff;
    return fb_getpixel(x, y) == fb_rgb((uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v);
}

static bool console_shows(const char *s) {
    uint32_t cols, rows;
    con_get_size(&cols, &rows);
    size_t n = strlen(s);
    for (uint32_t r = 0; r < rows; r++) {
        size_t i = 0;
        while (i < n && i < cols && con_cell_matches((uint32_t)i, r, s[i])) i++;
        if (i == n) return true;
    }
    return false;
}

void test_fbdev(void) {
    if (!con_active()) { kprintf("[TEST] fbdev skipped: no framebuffer\n"); return; }
    unsigned live0 = proc_live_count();
    uint64_t frames0 = pmm_free_frames();
    int err = 0;
    const char *argv[] = { "demo", "--test", NULL };
    struct process *p = proc_spawn_elf("/bin/demo", argv, &err);
    if (!p) panic("spawning demo failed (%d)", err);
    uint64_t end = timer_ms() + 3000;
    while (!(fbdev_owner() == p->pid && fb_pattern_ok())) {
        if (timer_ms() > end || p->exited) panic("fbdev: test pattern never appeared");
        thread_sleep_ms(5);
    }
    if (!con_suspended()) panic("fbdev: console not suspended while the screen is owned");
    const char *hidden = "fbdev: printed while hidden";
    kprintf("%s\n", hidden);
    if (!fb_pattern_ok()) panic("fbdev: console drew over the owned screen");
    int code = proc_wait(p, NULL);
    if (code) panic("demo --test failed check at line %d", code);
    if (fbdev_owner() != 0 || con_suspended()) panic("fbdev: screen not released at exit");
    if (fb_pattern_ok()) panic("fbdev: console not repainted");
    if (!console_shows(hidden)) panic("fbdev: text printed while hidden is missing");
    wait_live(live0);
    uint64_t nb, np;
    fbdev_stats(&nb, &np);
    kprintf("[TEST] fbdev ok: %ux%u, %lu blits, released at exit, console repainted, "
            "frames %s\n", g_fb.width, g_fb.height, nb,
            pmm_free_frames() == frames0 ? "back" : "LEAKED");
    if (pmm_free_frames() != frames0) panic("fbdev: frames leaked");
}
