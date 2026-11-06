/* /bin/killtest: checks kill(), interrupted blocking calls, spawn with redirected fds,
 * proclist and sysinfo. Exit code = number of the first failed check (0 = all ok).
 *   killtest waiter   (helper) spawns "child sleep 300" and waits for it
 *   killtest self     (helper) kills itself; must never return from kill() */
#include "tern.h"

#define CHECK(n, cond) do { if (!(cond)) { printf("killtest: check %d failed\n", n); return n; } } while (0)

static int run_status(int pid) {
    int st = -1;
    return wait(pid, &st) == pid ? st : -1000;
}

static bool read_file(const char *path, char *buf, size_t max) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    long n = read(fd, buf, max - 1);
    close(fd);
    if (n < 0) return false;
    buf[n] = 0;
    return true;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "waiter")) {
        const char *av[] = { "child", "sleep", "300", NULL };
        int c = spawn("/bin/child", av);
        if (c < 0) return 50;
        int st;
        int r = wait(c, &st);
        return r == -EINTR ? 51 : 52;   /* we should be killed before returning */
    }
    if (argc > 1 && !strcmp(argv[1], "self")) {
        kill(getpid());
        return 60;                       /* not reached */
    }

    /* 1. a ring-3 busy loop is killed at its next interrupt */
    const char *spin_av[] = { "spin", NULL };
    int pid = spawn("/bin/spin", spin_av);
    CHECK(1, pid > 0);
    sleep_ms(50);
    CHECK(1, kill(pid) == 0);
    CHECK(1, run_status(pid) == TERN_EXIT_KILLED);
    CHECK(1, kill(pid) == -ESRCH);       /* reaped: gone */

    /* 2. a sleeping process is woken and killed at once */
    const char *sl_av[] = { "child", "sleep", "10000", NULL };
    pid = spawn("/bin/child", sl_av);
    CHECK(2, pid > 0);
    sleep_ms(20);
    uint64_t t0 = uptime_ms();
    CHECK(2, kill(pid) == 0);
    CHECK(2, run_status(pid) == TERN_EXIT_KILLED);
    uint64_t sleep_kill_ms = uptime_ms() - t0;
    CHECK(2, sleep_kill_ms < 200);

    /* 3. a process blocked in wait() is interrupted (its child becomes an orphan) */
    const char *w_av[] = { "killtest", "waiter", NULL };
    pid = spawn("/bin/killtest", w_av);
    CHECK(3, pid > 0);
    sleep_ms(50);
    t0 = uptime_ms();
    CHECK(3, kill(pid) == 0);
    CHECK(3, run_status(pid) == TERN_EXIT_KILLED);
    CHECK(3, uptime_ms() - t0 < 200);

    /* 4. killing yourself takes effect on the way back from the system call */
    const char *s_av[] = { "killtest", "self", NULL };
    pid = spawn("/bin/killtest", s_av);
    CHECK(4, pid > 0 && run_status(pid) == TERN_EXIT_KILLED);

    /* 5. bad pids */
    CHECK(5, kill(0) == -ESRCH && kill(-1) == -ESRCH && kill(999999) == -ESRCH);

    /* 6. spawn with redirected fds: stdout to a file, then stdin from it */
    int out = open("/killtest.out", O_WRONLY | O_CREAT | O_TRUNC);
    CHECK(6, out >= 0);
    const char *e_av[] = { "echo", "redirected", "output", NULL };
    int fds[3] = { -1, out, -1 };
    pid = spawn_fds("/bin/echo", e_av, fds);
    CHECK(6, pid > 0 && run_status(pid) == 0);
    close(out);
    char buf[64];
    CHECK(6, read_file("/killtest.out", buf, sizeof buf) && !strcmp(buf, "redirected output\n"));
    int in = open("/killtest.out", O_RDONLY);
    out = open("/killtest.wc", O_WRONLY | O_CREAT | O_TRUNC);
    CHECK(6, in >= 0 && out >= 0);
    const char *wc_av[] = { "wc", NULL };
    int fds2[3] = { in, out, out };
    pid = spawn_fds("/bin/wc", wc_av, fds2);
    CHECK(6, pid > 0 && run_status(pid) == 0);
    close(in);
    close(out);
    CHECK(6, read_file("/killtest.wc", buf, sizeof buf) && !strcmp(buf, "1 2 18\n"));
    int bad[3] = { -1, 15, -1 };
    CHECK(6, spawn_fds("/bin/echo", e_av, bad) == -EBADF);
    bad[1] = -7;
    CHECK(6, spawn_fds("/bin/echo", e_av, bad) == -EBADF);
    CHECK(6, spawn_fds("/bin/echo", e_av, (const int *)0xffff800000000000) == -EFAULT);
    CHECK(6, unlink("/killtest.out") == 0 && unlink("/killtest.wc") == 0);

    /* 7. proclist finds us running; sysinfo is plausible */
    struct tern_procinfo pi[64];
    int n = proclist(pi, 64);
    CHECK(7, n >= 1 && n <= 64);
    bool me = false;
    for (int i = 0; i < n; i++)
        if (pi[i].pid == getpid() && pi[i].state == TERN_PS_RUNNING &&
            !strcmp(pi[i].name, "killtest") && pi[i].ppid == getppid()) me = true;
    CHECK(7, me);
    CHECK(7, proclist(pi, 0) == n);
    CHECK(7, proclist((struct tern_procinfo *)8, 4) == -EFAULT);
    struct tern_sysinfo si;
    CHECK(7, sysinfo(&si) == 0);
    CHECK(7, si.frames_free > 0 && si.frames_free < si.frames_total);
    CHECK(7, si.procs >= 1 && si.threads >= 3 && si.heap_used > 0 && si.vfs_nodes > 10);
    CHECK(7, sysinfo((struct tern_sysinfo *)0) == -EFAULT);

    printf("[TEST] kill ok: spinner, sleeper (%lu ms), waiter, self killed with 137; "
           "spawn fds redirect; proclist %d procs; sysinfo\n",
           (unsigned long)sleep_kill_ms, n);
    return 0;
}
