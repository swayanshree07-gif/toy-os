/* /bin/exectest: exec() and waitpid(WNOHANG) from ring 3 (Day 12 session).
 *   exectest                 checks failing execs and WNOHANG, then execs itself as
 *                            "exectest stage2 PID PPID FD CHILD"
 *   exectest stage2 ...      checks that pid, parent, fds, cwd and children survived and
 *                            that the image is fresh, then execs "/bin/child exit 77"
 * So the final exit code is 77 when everything passed; a failed check exits with its
 * number (1..49 in the first stage, 50.. in the second). */
#include "tern.h"

static int check_no;
#define CHECK(c) do { check_no++; if (!(c)) { \
    printf("exectest: check %d failed: %s\n", check_no, #c); exit(check_no); } } while (0)

static int fresh = 1234;               /* .data: must be back to 1234 after an exec */
static char big[8192];                 /* .bss: must be zero again */

static int stage1(void) {
    int me = getpid(), st = -1;
    /* failing execs return and leave us untouched */
    fresh = 99;
    big[100] = 'x';
    CHECK(exec("/nowhere", NULL) == -ENOENT);
    CHECK(exec("/etc/motd", NULL) == -ENOEXEC);
    CHECK(exec("/bin", NULL) == -EACCES);
    CHECK(exec((const char *)0x10, NULL) == -EFAULT);
    const char *bad_argv[] = { "x", (const char *)0xdead0000, NULL };
    CHECK(exec("/bin/hello", bad_argv) == -EFAULT);
    CHECK(fresh == 99 && big[100] == 'x' && getpid() == me);

    /* waitpid with WNOHANG */
    CHECK(waitpid(-1, &st, TERN_WNOHANG) == -ECHILD);
    const char *sav[] = { "child", "sleep", "150", NULL };
    int pid = spawn("/bin/child", sav);
    CHECK(pid > 0);
    CHECK(waitpid(pid, &st, TERN_WNOHANG) == 0);
    CHECK(waitpid(pid, &st, 2) == -EINVAL);
    CHECK(waitpid(pid, &st, 0) == pid && st == 7);
    CHECK(waitpid(pid, &st, TERN_WNOHANG) == -ECHILD);
    const char *eav[] = { "child", "exit", "9", NULL };
    pid = spawn("/bin/child", eav);
    CHECK(pid > 0);
    int got = 0;
    for (int i = 0; i < 1000 && got == 0; i++) {   /* poll until it has exited */
        got = waitpid(-1, &st, TERN_WNOHANG);
        if (!got) sleep_ms(1);
    }
    CHECK(got == pid && st == 9);

    /* state that must survive the exec: an open fd, the cwd, a running child */
    int fd = open("/etc/motd", O_RDONLY);
    CHECK(fd >= 3);
    /* dup/dup2 share the open file (and its offset) */
    int d1 = dup(fd);
    CHECK(d1 > fd);
    CHECK(lseek(d1, 5, SEEK_SET) == 5 && lseek(fd, 0, SEEK_CUR) == 5);
    CHECK(dup2(fd, 15) == 15 && lseek(15, 0, SEEK_CUR) == 5);
    CHECK(dup2(fd, 16) == -EBADF && dup(99) == -EBADF && dup2(fd, fd) == fd);
    CHECK(dup2(d1, 15) == 15);                          /* replaces an open fd */
    CHECK(close(d1) == 0 && close(15) == 0 && lseek(fd, 0, SEEK_CUR) == 5);
    CHECK(lseek(fd, 3, SEEK_SET) == 3);
    CHECK(chdir("/etc") == 0);
    const char *lav[] = { "child", "sleep", "100", NULL };
    int kid = spawn("/bin/child", lav);
    CHECK(kid > 0);
    char a[16], b[16], c[16], d[16];
    snprintf(a, sizeof a, "%d", me);
    snprintf(b, sizeof b, "%d", getppid());
    snprintf(c, sizeof c, "%d", fd);
    snprintf(d, sizeof d, "%d", kid);
    const char *av[] = { "exectest", "stage2", a, b, c, d, NULL };
    uint32_t mx = 0x9FC0;                               /* FTZ + DAZ: must not survive */
    __asm__ volatile("ldmxcsr %0" : : "m"(mx));
    int rc = exec("/bin/exectest", av);
    printf("exectest: exec returned %d\n", rc);
    return 49;
}

static int stage2(char **argv) {
    check_no = 49;
    int fd = atoi(argv[4]), st = -1;
    CHECK(getpid() == atoi(argv[2]));
    CHECK(getppid() == atoi(argv[3]));
    CHECK(fresh == 1234 && big[100] == 0);              /* new image */
    CHECK(lseek(fd, 0, SEEK_CUR) == 3);                 /* same open file, same offset */
    char cwd[64];
    CHECK(getcwd(cwd, sizeof cwd) > 0 && !strcmp(cwd, "/etc"));
    CHECK(wait(-1, &st) == atoi(argv[5]) && st == 7);   /* our child is still ours */
    /* the FPU starts clean: default MXCSR */
    uint32_t mxcsr = 0;
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    CHECK(mxcsr == 0x1F80);
    close(fd);
    printf("exectest: stage2 ok (pid %d), exec child\n", getpid());
    const char *av[] = { "child", "exit", "77", NULL };
    exec("/bin/child", av);
    return 99;
}

int main(int argc, char **argv) {
    if (argc == 6 && !strcmp(argv[1], "stage2")) return stage2(argv);
    return stage1();
}
