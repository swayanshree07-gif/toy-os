/* /bin/fuzz [ITER] [SEED]: system-call fuzzer (plan Day 26 robustness). Calls random
 * system call numbers (including invalid ones) with arguments drawn from a pool of
 * nasty values: NULL, kernel addresses, non-canonical and unmapped addresses, pointers
 * just before the end of mapped memory, huge lengths, negative numbers and small fds.
 * The kernel must reject everything without crashing or leaking. Calls that would end
 * or block the fuzzer (exit, kill of others, sleep, reads of the console, wait for
 * nothing) are filtered; spawn is limited to bad arguments.
 * Prints "[TEST] fuzz ok: N calls, ..." and exits 0. */
#include "tern.h"

static uint64_t rng;
static uint64_t rnd(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return rng;
}

static char page_buf[8192] __attribute__((aligned(4096)));
/* Syscalls that write to user memory get pointers into out_buf instead, so their
 * output (e.g. "/fuzz" from getcwd, or binary '/' bytes) never becomes a path that
 * escapes /fuzz. Path strings come only from page_buf (letters and NULs). */
static char out_buf[8192] __attribute__((aligned(4096)));
static uint64_t to_out(uint64_t p) {
    uint64_t base = (uint64_t)page_buf;
    return p >= base && p < base + sizeof page_buf ? p - base + (uint64_t)out_buf : p;
}

/* rm -r, depth-limited */
static void remove_tree(const char *path, int depth) {
    if (depth > 8) return;
    int d = open(path, O_RDONLY | O_DIRECTORY);
    if (d >= 0) {
        struct tern_dirent de;
        char sub[160];
        while (readdir(d, &de) == 1) {
            snprintf(sub, sizeof sub, "%s/%s", path, de.name);
            remove_tree(sub, depth + 1);
            lseek(d, 0, SEEK_SET);           /* entries shift after an unlink */
        }
        close(d);
    }
    unlink(path);
}

static uint64_t pick_arg(void) {
    static const uint64_t fixed[] = {
        0, 1, 2, 3, 15, 16, 17, 255, 4096, 0x7fffffff, 0x80000000ull,
        0xffffffffull, 0xffffffffffffffffull, 0xfffffffffffffff0ull, 0x8000000000000000ull,
        0x00007ffffffff000ull, 0x00007fffffffffffull, 0x0000800000000000ull,   /* edge/non-canonical */
        0xffff800000000000ull, 0xffffffff80100000ull, 0xffffff1000000000ull,   /* kernel */
        0x400000, 0x1000, 0x10, 0x3fffff,
    };
    switch (rnd() % 6) {
    case 0: case 1: return fixed[rnd() % (sizeof fixed / sizeof fixed[0])];
    case 2: return (uint64_t)page_buf + rnd() % sizeof page_buf;          /* valid memory */
    case 3: return (uint64_t)page_buf + sizeof page_buf - rnd() % 16;      /* near the end */
    case 4: return rnd() % 64;
    default: return rnd();
    }
}

static long raw3(long nr, uint64_t a, uint64_t b, uint64_t c) {
    return tern_syscall3(nr, (long)a, (long)b, (long)c);
}

int main(int argc, char **argv) {
    uint64_t t_start = uptime_ms();
    long iters = argc > 1 ? atoi(argv[1]) : 20000;
    rng = argc > 2 ? (uint64_t)atoi(argv[2]) : 0x7e57f0221ull;
    if (!rng) rng = 1;
    /* fds 0-2 become /dev/null so fuzzed reads/writes don't block or spam the console */
    for (int i = 0; i < 3; i++) close(i);
    for (int i = 0; i < 3; i++) if (open("/dev/null", O_RDWR) != i) return 90;
    mkdir("/fuzz");
    chdir("/fuzz");
    long calls = 0, errors = 0, ok = 0, enosys = 0, efault = 0;
    long self = getpid();
    for (long it = 0; it < iters; it++) {
        long nr = (long)(rnd() % (SYS_COUNT + 4));
        if (rnd() % 50 == 0) nr = (long)rnd();                       /* wild numbers */
        uint64_t a = pick_arg(), b = pick_arg(), c = pick_arg();
        /* keep randomly written bytes in our buffer NUL-terminated now and then so
         * path syscalls see short garbage names too */
        page_buf[rnd() % sizeof page_buf] = (char)(rnd() % 3 ? 0 : 'a' + rnd() % 26);
        switch (nr) {
        case SYS_exit: continue;
        case SYS_sleep_ms: a = 0; break;                   /* (a tick each would be slow) */
        case SYS_kill: if (a != (uint64_t)self) a = 0x7ffffff0; else continue; break;
        case SYS_setfg: continue;                          /* would steal Ctrl-C */
        case SYS_wait:                                     /* no children: -ECHILD */
            a = (uint64_t)-1; b = to_out(b); if (rnd() % 2) c &= TERN_WNOHANG; break;
        case SYS_spawn: case SYS_exec:                                    /* only paths that can't exist */
            if (a >= (uint64_t)page_buf && a < (uint64_t)page_buf + sizeof page_buf) continue;
            break;
        case SYS_brk: {                    /* small moves around the current break */
            long cur = brk(0);
            a = rnd() % 4 ? (uint64_t)(cur + (long)(rnd() % 65536) - 32768) : 0;
            break;
        }
        case SYS_read: case SYS_write: case SYS_readdir: case SYS_close: case SYS_lseek:
        case SYS_fstat:
            if (a < 3 && nr == SYS_close) continue;        /* keep /dev/null on 0-2 */
            if (nr == SYS_read || nr == SYS_readdir || nr == SYS_fstat) b = to_out(b);
            break;
        case SYS_dup2: if (b < 3) continue; break;         /* keep /dev/null on 0-2 */
        case SYS_chdir: continue;                          /* stay in /fuzz */
        case SYS_poweroff: continue;                       /* would end the test run */
        case SYS_fbinfo: a = to_out(a); break;
        case SYS_getcwd: case SYS_sysinfo: case SYS_proclist: case SYS_pipe:
            a = to_out(a); break;
        case SYS_stat: b = to_out(b); break;
        default: break;
        }
        long r = raw3(nr, a, b, c);
        calls++;
        if (r < 0) {
            errors++;
            if (r == -ENOSYS) enosys++;
            if (r == -EFAULT) efault++;
        } else {
            ok++;
        }
        /* close whatever the fuzzer opened, so EMFILE doesn't swallow everything */
        if ((nr == SYS_open || nr == SYS_dup || nr == SYS_dup2) && r >= 3) close((int)r);
        if (nr == SYS_pipe && r == 0) for (int i = 3; i < 16; i++) close(i);
    }
    for (int i = 3; i < 16; i++) close(i);
    remove_tree("/fuzz", 0);
    chdir("/");
    int fd = open("/dev/console", O_WRONLY);
    struct tern_stat fst;
    int urc = stat("/fuzz", &fst) == -ENOENT ? 0 : -1;
    if (urc) {                                  /* say what is left */
        int d = open("/fuzz", O_RDONLY | O_DIRECTORY);
        struct tern_dirent de;
        while (d >= 0 && readdir(d, &de) == 1) {
            dprintf(fd, "fuzz: left: type %u size %lu name", de.type, (unsigned long)de.size);
            for (int i = 0; de.name[i]; i++) dprintf(fd, " %02x", (unsigned char)de.name[i]);
            dprintf(fd, "\n");
        }
    }

    dprintf(fd, "[TEST] fuzz ok: %ld calls, %ld ok, %ld errors (%ld ENOSYS, %ld EFAULT), "
            "/fuzz %s, %lu ms\n", calls, ok, errors, enosys, efault, urc == 0 ? "removed" : "left",
            (unsigned long)(uptime_ms() - t_start));
    return 0;
}
