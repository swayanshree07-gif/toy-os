/* /bin/pipetest: ring-3 checks for pipes, process groups, rename and the process
 * limit (Day 10 session). Exit code = number of the first failed check (0 = ok);
 * prints one summary line at the end. */
#include "tern.h"

#define CHECK(n, cond) do { if (!(cond)) { printf("pipetest: check %d failed (line %d)\n", n, __LINE__); return n; } } while (0)

static int status_of(int pid) {
    int st = -1;
    return wait(pid, &st) == pid ? st : -1000;
}

/* spawn argv with the given fds for 0/1/2 */
static int run(const char *const *av, int in, int out) {
    char path[64];
    snprintf(path, sizeof path, "/bin/%s", av[0]);
    int fds[3] = { in, out, -1 };
    return spawn_fds(path, av, fds);
}

static int state_of(int pid) {
    static struct tern_procinfo pi[64];
    int n = proclist(pi, 64);
    for (int i = 0; i < n && i < 64; i++) if (pi[i].pid == pid) return (int)pi[i].state;
    return -1;
}

static int open_fds(void) {          /* how many fds are open (EBADF probe via fstat) */
    struct tern_stat st;
    int n = 0;
    for (int i = 0; i < 16; i++) if (fstat(i, &st) == 0) n++;
    return n;
}

int main(void) {
    struct tern_sysinfo si0, si1;
    sysinfo(&si0);
    int fds0 = open_fds();

    /* 1. basics: data round trip, ends are one-way, fstat says device */
    int p[2] = { -1, -1 };
    CHECK(1, pipe(p) == 0 && p[0] >= 3 && p[1] > p[0]);
    CHECK(1, write(p[1], "hello", 5) == 5);
    char buf[8192];
    CHECK(1, read(p[0], buf, sizeof buf) == 5 && !memcmp(buf, "hello", 5));
    CHECK(1, write(p[0], "x", 1) == -EBADF && read(p[1], buf, 1) == -EBADF);
    struct tern_stat st;
    CHECK(1, fstat(p[0], &st) == 0 && st.type == TERN_FT_DEV);
    CHECK(1, lseek(p[0], 0, SEEK_SET) == -EINVAL);
    CHECK(1, pipe((int *)0) == -EFAULT && pipe((int *)0xffffffff80100000ull) == -EFAULT);

    /* 2. end of file once the writer is closed, after the buffered data */
    CHECK(2, write(p[1], "abc", 3) == 3);
    close(p[1]);
    CHECK(2, read(p[0], buf, 2) == 2 && read(p[0], buf + 2, 10) == 1 && !memcmp(buf, "abc", 3));
    CHECK(2, read(p[0], buf, 10) == 0 && read(p[0], buf, 10) == 0);
    close(p[0]);

    /* 3. EPIPE once the reader is closed */
    CHECK(3, pipe(p) == 0);
    close(p[0]);
    CHECK(3, write(p[1], "x", 1) == -EPIPE);
    close(p[1]);

    /* 4. a full buffer: 4096 bytes fit, a big write from a child blocks until we read.
     *    "seq 1 20000" writes 108894 bytes through the 4 KiB pipe. */
    CHECK(4, pipe(p) == 0);
    memset(buf, 'z', 4096);
    CHECK(4, write(p[1], buf, 4096) == 4096);
    CHECK(4, read(p[0], buf, sizeof buf) == 4096);
    const char *seq_av[] = { "seq", "1", "20000", NULL };
    int pid = run(seq_av, -1, p[1]);
    CHECK(4, pid > 0);
    close(p[1]);                       /* only the child writes now */
    long total = 0, n, lines = 0, sum = 0, cur = 0;
    while ((n = read(p[0], buf, sizeof buf)) > 0) {
        for (long i = 0; i < n; i++) {
            if (buf[i] == '\n') { lines++; sum += cur; cur = 0; }
            else cur = cur * 10 + (buf[i] - '0');
        }
        total += n;
    }
    CHECK(4, n == 0 && status_of(pid) == 0);
    CHECK(4, total == 108894 && lines == 20000 && sum == 200010000L);
    close(p[0]);

    /* 5. a pipeline of children: seq 1 500 | grep 7 | wc  (wc's output to our pipe) */
    int a[2], b[2], c[2];
    CHECK(5, pipe(a) == 0 && pipe(b) == 0 && pipe(c) == 0);
    const char *s5[] = { "seq", "500", NULL }, *g5[] = { "grep", "7", NULL }, *w5[] = { "wc", NULL };
    int p1 = run(s5, -1, a[1]), p2 = run(g5, a[0], b[1]), p3 = run(w5, b[0], c[1]);
    CHECK(5, p1 > 0 && p2 > 0 && p3 > 0);
    close(a[0]); close(a[1]); close(b[0]); close(b[1]); close(c[1]);
    n = read(c[0], buf, sizeof buf - 1);
    CHECK(5, n > 0);
    buf[n] = 0;
    /* numbers 1..500 containing a 7: 95 of them, 3 chars or less each */
    CHECK(5, !strncmp(buf, "95 95 ", 6));
    CHECK(5, status_of(p1) == 0 && status_of(p2) == 0 && status_of(p3) == 0);
    close(c[0]);

    /* 6. yes into a pipe whose reader goes away exits cleanly (EPIPE) */
    CHECK(6, pipe(p) == 0);
    const char *yes_av[] = { "yes", "tern", NULL };
    pid = run(yes_av, -1, p[1]);
    CHECK(6, pid > 0);
    close(p[1]);
    CHECK(6, read(p[0], buf, 5) == 5 && !memcmp(buf, "tern\n", 5));
    close(p[0]);
    CHECK(6, status_of(pid) == 0);

    /* 7. kill wakes a process blocked reading an empty pipe and one blocked writing a
     *    full pipe */
    const char *cat_av[] = { "cat", NULL };
    int rd_pid, wr_pid;
    uint64_t t0;
    /* blocked reader with a writer still open (the pipe stays empty) */
    CHECK(7, pipe(p) == 0);
    rd_pid = run(cat_av, p[0], -1);
    CHECK(7, rd_pid > 0);
    sleep_ms(30);
    CHECK(7, state_of(rd_pid) == TERN_PS_BLOCKED);
    t0 = uptime_ms();
    CHECK(7, kill(rd_pid) == 0 && status_of(rd_pid) == TERN_EXIT_KILLED && uptime_ms() - t0 < 100);
    /* blocked writer: a full pipe nobody reads */
    wr_pid = run(yes_av, -1, p[1]);
    CHECK(7, wr_pid > 0);
    sleep_ms(30);
    CHECK(7, state_of(wr_pid) == TERN_PS_BLOCKED);
    t0 = uptime_ms();
    CHECK(7, kill(wr_pid) == 0 && status_of(wr_pid) == TERN_EXIT_KILLED && uptime_ms() - t0 < 100);
    close(p[0]);
    close(p[1]);

    /* 8. EMFILE: pipes until the fd table is full; nothing leaks */
    int fds[16], nf = 0, rc;
    while ((rc = pipe(&fds[nf])) == 0) nf += 2;
    CHECK(8, rc == -EMFILE && nf >= 10);
    for (int i = 0; i < nf; i++) close(fds[i]);
    CHECK(8, open_fds() == fds0);

    /* 9. process groups */
    int me = getpid();
    CHECK(9, getpgid(0) == me && getpgid(me) == me && getpgid(999999) == -ESRCH);
    CHECK(9, setpgid(1, 1) == -ESRCH || getppid() == 1);   /* not our child */
    const char *sl_av[] = { "child", "sleep", "5000", NULL };
    int c1 = spawn("/bin/child", sl_av), c2 = spawn("/bin/child", sl_av);
    CHECK(9, c1 > 0 && c2 > 0 && getpgid(c1) == c1);
    CHECK(9, setpgid(c2, c1) == 0 && getpgid(c2) == c1);
    /* (killed right after spawn, maybe before they ran: the kill must still land at
     * once, not when their 5 s sleep ends) */
    t0 = uptime_ms();
    CHECK(9, kill(c1) == 0 && kill(c2) == 0);
    CHECK(9, status_of(c1) == TERN_EXIT_KILLED && status_of(c2) == TERN_EXIT_KILLED);
    CHECK(9, uptime_ms() - t0 < 200);
    CHECK(9, setpgid(c1, c1) == -ESRCH);                  /* reaped */

    /* 10. rename */
    mkdir("/rn");
    int fd = open("/rn/a", O_WRONLY | O_CREAT);
    CHECK(10, fd >= 0 && write(fd, "data", 4) == 4);
    close(fd);
    CHECK(10, rename("/rn/a", "/rn/b") == 0 && stat("/rn/a", &st) == -ENOENT);
    CHECK(10, stat("/rn/b", &st) == 0 && st.size == 4);
    CHECK(10, mkdir("/rn/d") == 0 && rename("/rn/b", "/rn/d/c") == 0);
    CHECK(10, chdir("/rn/d") == 0 && rename("c", "../e") == 0 && chdir("/") == 0);
    CHECK(10, stat("/rn/e", &st) == 0 && st.size == 4);
    CHECK(10, rename("/rn", "/rn/d/x") == -EINVAL);       /* into its own subtree */
    CHECK(10, rename("/rn/d", "/rn/d") == 0);             /* no-op */
    CHECK(10, rename("/rn/e", "/rn/d") == -EISDIR);
    CHECK(10, rename("/rn/d", "/rn/e") == -ENOTDIR);
    CHECK(10, rename("/nope", "/rn/z") == -ENOENT && rename("/rn/e", "/nope/z") == -ENOENT);
    CHECK(10, rename("/", "/rn/root") == -EINVAL || rename("/", "/rn/root") == -EBUSY);
    fd = open("/rn/f", O_WRONLY | O_CREAT);
    close(fd);
    CHECK(10, rename("/rn/e", "/rn/f") == 0 && stat("/rn/f", &st) == 0 && st.size == 4);  /* replaced */
    CHECK(10, mkdir("/rn/d/sub") == 0 && mkdir("/rn/g") == 0);
    CHECK(10, rename("/rn/g", "/rn/d") == -ENOTEMPTY && rename("/rn/d", "/rn/g") == 0);
    CHECK(10, stat("/rn/g/sub", &st) == 0 && st.type == TERN_FT_DIR);
    CHECK(10, rename((const char *)8, "/x") == -EFAULT);
    CHECK(10, unlink("/rn/g/sub") == 0 && unlink("/rn/g") == 0 && unlink("/rn/f") == 0 && unlink("/rn") == 0);

    /* 11. the process limit: spawn sleepers until -EAGAIN, then everything is reclaimed */
    static int kids[80];
    int nk = 0;
    const char *long_av[] = { "child", "sleep", "20000", NULL };
    while (nk < 80 && (rc = spawn("/bin/child", long_av)) > 0) kids[nk++] = rc;
    CHECK(11, rc == -EAGAIN && nk >= 40 && nk < PROC_MAX_LIVE);
    t0 = uptime_ms();
    for (int i = 0; i < nk; i++) kill(kids[i]);
    for (int i = 0; i < nk; i++) CHECK(11, status_of(kids[i]) == TERN_EXIT_KILLED);
    CHECK(11, uptime_ms() - t0 < 2000);
    const char *ex_av[] = { "child", "exit", "9", NULL };
    pid = spawn("/bin/child", ex_av);
    CHECK(11, pid > 0 && status_of(pid) == 9);

    sysinfo(&si1);
    CHECK(12, si1.pipes_live == si0.pipes_live && open_fds() == fds0);
    printf("[TEST] pipes ok: eof/epipe/ebadf, %ld bytes seq->pipe, 3-stage pipeline, kill "
           "while blocked, emfile, pgid, rename, %d spawns then EAGAIN; %lu pipes created\n",
           total, nk, (unsigned long)(si1.pipes_created - si0.pipes_created));
    return 0;
}
