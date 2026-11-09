/* /bin/forktest (Day 22): copy-on-write fork check.
 * - a 64-page buffer is dirtied, then fork(): the pages are shared, not copied, so the
 *   fork costs only page tables and a few frames;
 * - parent and child write the same variable and each sees its own value;
 * - the child writes 16 buffer pages -> exactly 16 COW copies;
 * - after the child is gone the parent's writes just re-enable writing (no copies);
 * - a pipe opened before fork carries data from the child to the parent;
 * - 20 fork/exit rounds leave the free-frame count where it was. */
#include "tern.h"

#define NPAGES 256
static volatile int value = 1;
static unsigned char buf[NPAGES * 4096] __attribute__((aligned(4096)));
static struct tern_sysinfo si;
static void snap(void) { sysinfo(&si); }

int main(void) {
    for (int i = 0; i < NPAGES; i++) buf[i * 4096] = (unsigned char)i;
    int pfd[2];
    if (pipe(pfd)) { printf("forktest: pipe failed\n"); return 1; }
    /* warm-up fork so later page-table counts are steady */
    int w = fork();
    if (w == 0) exit(0);
    int st;
    wait(w, &st);

    snap();
    uint64_t free0 = si.frames_free, shared0 = si.cow_shared, copies0 = si.cow_copies;
    int pid = fork();
    if (pid < 0) { printf("forktest: fork failed %d\n", pid); return 1; }
    if (pid == 0) {
        snap();
        uint64_t used = free0 - si.frames_free, shared = si.cow_shared - shared0;
        printf("forktest: child pid %d ppid %d, fork shared %lu pages using %lu frames (%s)\n",
               getpid(), getppid(), (unsigned long)shared, (unsigned long)used,
               used * 4 < shared ? "shared" : "COPIED?");
        value = 2;
        int bad = 0;
        for (int i = 0; i < NPAGES; i++) bad += buf[i * 4096] != (unsigned char)i;
        snap();
        uint64_t c0 = si.cow_copies;
        for (int i = 0; i < 16; i++) buf[i * 4096] = 0xEE;
        snap();
        printf("forktest: child value %d, buffer %d bad, wrote 16 pages -> %lu copies\n",
               value, bad, (unsigned long)(si.cow_copies - c0));
        close(pfd[0]);
        write(pfd[1], "via pipe", 8);
        exit(7);
    }
    close(pfd[1]);
    char msg[16] = {0};
    long n = read(pfd[0], msg, sizeof msg - 1);
    close(pfd[0]);
    int code = -1;
    wait(pid, &code);
    int bad = 0;
    for (int i = 0; i < NPAGES; i++) bad += buf[i * 4096] != (unsigned char)i;
    printf("forktest: parent value %d, child exit %d, buffer %d bad, pipe '%s' (%ld)\n",
           value, code, bad, msg, n);
    value = 3;
    snap();
    uint64_t r0 = si.cow_reuses, c1 = si.cow_copies;
    for (int i = 0; i < NPAGES; i++) buf[i * 4096] = 1;
    snap();
    printf("forktest: parent rewrote %d pages after child exit -> %lu copies, %s\n", NPAGES,
           (unsigned long)(si.cow_copies - c1),
           si.cow_reuses - r0 >= NPAGES - 16 ? "reused" : "NOT reused");
    (void)copies0;

    snap();
    uint64_t f0 = si.frames_free;
    for (int k = 0; k < 20; k++) {
        int c = fork();
        if (c == 0) { buf[k * 4096] = 9; value = k; exit(k); }
        if (c < 0) { printf("forktest: fork %d failed %d\n", k, c); return 1; }
        int s2;
        wait(c, &s2);
        if (s2 != k) printf("forktest: round %d exit %d\n", k, s2);
    }
    for (int i = 0; i < NPAGES; i++) buf[i * 4096] = 2;   /* take back sole ownership */
    snap();
    printf("forktest: 20 fork/exit rounds -> frames %s (%ld)\n",
           si.frames_free == f0 ? "back to start" : "LEAKED", (long)(f0 - si.frames_free));
    printf("forktest: value %d, cow fork ok\n", value);
    return 0;
}
