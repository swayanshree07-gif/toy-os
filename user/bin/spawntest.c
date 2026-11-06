/* /bin/spawntest: spawn/wait/getppid/chdir/getcwd from ring 3 (plan Day 21).
 * Exits 0 if every check passes, otherwise with the number of the failed check. */
#include "tern.h"

static int check_no;
#define CHECK(c) do { check_no++; if (!(c)) { \
    printf("spawntest: check %d failed: %s\n", check_no, #c); exit(check_no); } } while (0)

static int run(const char *a1, const char *a2) {
    const char *av[] = { "child", a1, a2, NULL };
    return spawn("/bin/child", av);
}

int main(void) {
    int me = getpid(), st = -1;
    char num[16];

    /* spawn + wait for a specific child, exit code comes back */
    const char *hav[] = { "hello", "from", "spawntest", NULL };
    int pid = spawn("/bin/hello", hav);
    CHECK(pid > me);
    CHECK(wait(pid, &st) == pid && st == 42);
    CHECK(wait(pid, &st) == -ECHILD);            /* already reaped */

    /* the child sees us as its parent */
    snprintf(num, sizeof num, "%d", me);
    pid = run("ppid", num);
    CHECK(pid > 0 && wait(pid, &st) == pid && st == 0);

    /* several children, collected in any order with wait(-1) */
    int pids[4], seen = 0;
    for (int i = 0; i < 4; i++) {
        snprintf(num, sizeof num, "%d", 10 + i);
        pids[i] = run("exit", num);
        CHECK(pids[i] > 0);
    }
    for (int i = 0; i < 4; i++) {
        int got = wait(-1, &st);
        int k = 0;
        while (k < 4 && pids[k] != got) k++;
        CHECK(k < 4 && st == 10 + k && !(seen & (1 << k)));
        seen |= 1 << k;
    }
    CHECK(seen == 15);
    CHECK(wait(-1, &st) == -ECHILD);             /* no children left */
    CHECK(wait(12345, NULL) == -ECHILD);

    /* wait blocks until a sleeping child is done; NULL status is allowed */
    uint64_t t0 = uptime_ms();
    pid = run("sleep", "60");
    CHECK(pid > 0 && wait(pid, NULL) == pid && uptime_ms() - t0 >= 60);

    /* a faulting child reports 128 + vector (#PF = 14) */
    pid = spawn("/bin/segv", NULL);
    CHECK(pid > 0 && wait(pid, &st) == pid && st == 128 + 14);

    /* errors */
    CHECK(spawn("/bin/nope", NULL) == -ENOENT);
    CHECK(spawn("/bin", NULL) == -EACCES);
    CHECK(spawn((const char *)0xFFFFFFFF80100000ull, NULL) == -EFAULT);
    const char *badv[] = { "child", (const char *)16, NULL };
    CHECK(spawn("/bin/child", badv) == -EFAULT);
    CHECK(spawn("/bin/child", (const char *const *)8) == -EFAULT);
    CHECK(wait(-1, (int *)0xFFFFFFFF80100000ull) == -EFAULT);

    /* cwd: chdir/getcwd, inherited by children */
    char cwd[64];
    CHECK(getcwd(cwd, sizeof cwd) == 1 && !strcmp(cwd, "/"));
    mkdir("/tmp");                               /* may exist already */
    CHECK(mkdir("/tmp/sp") == 0);
    CHECK(chdir("/tmp/sp") == 0);
    CHECK(getcwd(cwd, sizeof cwd) == 7 && !strcmp(cwd, "/tmp/sp"));
    CHECK(getcwd(cwd, 7) == -ERANGE);
    CHECK(chdir("..") == 0 && getcwd(cwd, sizeof cwd) == 4 && !strcmp(cwd, "/tmp"));
    CHECK(chdir("/etc/motd") == -ENOTDIR);
    CHECK(chdir("/nope") == -ENOENT);
    CHECK(chdir("sp") == 0);
    pid = spawn("../../bin/child", (const char *const *)0);   /* relative to the cwd */
    CHECK(pid > 0 && wait(pid, &st) == pid && st == 100);    /* argc 1 -> 100 */
    CHECK(chdir("/") == 0 && unlink("/tmp/sp") == 0);

    /* orphans: the child exits at once, the grandchild outlives it and us */
    pid = run("orphan", "80");
    CHECK(pid > 0 && wait(pid, &st) == pid && st == 0);
    /* and a child we never wait for */
    CHECK(run("sleep", "40") > 0);

    printf("[TEST] wait ok: spawn/wait exit codes, wait(-1) x4, ECHILD, getppid, "
           "fault status %d, chdir/getcwd\n", 128 + 14);
    return 0;
}
