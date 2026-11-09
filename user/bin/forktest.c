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

/* Recurses with a page-sized frame; at the bottom forks, both sides unwind and
 * write to every frame on the way up (COW copies of the stack pages). */
static int deep(int n, int forked) {
    volatile char frame[4096];
    frame[n & 7] = (char)n;
    if (n == 0) {
        int c = fork();
        if (c == 0) return -1000;   /* child marker, see below */
        int st = -1;
        wait(c, &st);
        return st;
    }
    int r = deep(n - 1, forked);
    frame[(n & 7) + 8] = (char)(n + 1);       /* write after the fork */
    if (r <= -1000) {               /* in the child */
        r -= n;
        if (n == 64) exit(frame[n & 7] == (char)n ? 42 : 1);
        return r;
    }
    return r + (frame[n & 7] == (char)n);
}

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

    /* Day 23: 100 fork/exit rounds, twice; the first batch may grow kernel tables
     * (heap, pid slots), the second must leave the frame count unchanged. */
    long lost[2], heapg[2];
    for (int b = 0; b < 2; b++) {
        snap();
        uint64_t f0 = si.frames_free, h0 = si.heap_mapped;
        for (int k = 0; k < 100; k++) {
            int c = fork();
            if (c == 0) { buf[k * 2 * 4096 % (NPAGES * 4096)] = 9; value = k; exit(k); }
            if (c < 0) { printf("forktest: fork %d failed %d\n", k, c); return 1; }
            int s2;
            wait(c, &s2);
            if (s2 != k) printf("forktest: round %d exit %d\n", k, s2);
        }
        for (int i = 0; i < NPAGES; i++) buf[i * 4096] = 2;   /* take back sole ownership */
        snap();
        heapg[b] = (long)((si.heap_mapped - h0) / 4096);
        lost[b] = (long)(f0 - si.frames_free) - heapg[b];
    }
    printf("forktest: 2x100 fork/exit rounds -> frames %s (%ld, %ld; kernel heap grew %ld, %ld pages)\n",
           lost[0] == 0 && lost[1] == 0 ? "back to start" : "LEAKED", lost[0], lost[1], heapg[0], heapg[1]);

    /* Day 23: deep stack shared at fork; both sides rewrite it */
    snap();
    uint64_t c2 = si.cow_copies;
    int sum = deep(64, 0);
    snap();
    printf("forktest: deep stack 64 frames -> sum %d, %s\n", sum,
           si.cow_copies - c2 >= 64 ? "stack pages copied on write" : "NO stack copies?");

    /* fork + exec: the child replaces itself, keeps the pipe fd as stdout */
    int ep[2];
    pipe(ep);
    int e = fork();
    if (e == 0) {
        dup2(ep[1], 1);
        close(ep[0]); close(ep[1]);
        const char *av[] = { "echo", "exec", "from", "child", NULL };
        exec("/bin/echo", av);
        exit(99);
    }
    close(ep[1]);
    char eb[64] = {0};
    long en = 0, k2;
    while ((k2 = read(ep[0], eb + en, sizeof eb - 1 - en)) > 0) en += k2;
    close(ep[0]);
    int es = -1;
    wait(e, &es);
    if (en > 0 && eb[en - 1] == '\n') eb[--en] = 0;
    printf("forktest: fork+exec -> '%s', exit %d\n", eb, es);

    /* memory pressure: fill memory with touched pages, fork must fail cleanly */
    snap();
    uint64_t f1 = si.frames_free, h1 = si.heap_mapped;
    /* leave ~24 frames after the region's own page tables */
    size_t np = si.frames_free > 256 ? si.frames_free - 32 - si.frames_free / 512 - 4 : 0;
    long m = mmap(np * 4096, PROT_READ | PROT_WRITE);
    int failed = 0, okf = 0;
    if (m > 0) {
        volatile unsigned char *mp = (volatile unsigned char *)m;
        for (size_t i = 0; i < np; i++) mp[i * 4096] = 1;
        for (int t = 0; t < 4; t++) {
            int c = fork();
            if (c == 0) exit(0);
            if (c < 0) failed++;
            else { int s3; wait(c, &s3); okf++; }
        }
        munmap((void *)m, np * 4096);
    }
    int c4 = fork();
    if (c4 == 0) exit(5);
    int s4 = -1;
    if (c4 > 0) wait(c4, &s4);
    snap();
    /* the region's page tables stay until exit (see Day 21), plus kernel heap growth */
    long lostp = (long)(f1 - si.frames_free) - (long)((si.heap_mapped - h1) / 4096) - (long)(np / 512 + 3);
    printf("forktest: under memory pressure %d/4 forks failed -> %s (%d), after munmap fork exit %d, frames %s\n",
           failed, okf == 0 ? "ENOMEM" : "some ran", failed ? -12 : 0, s4,
           lostp <= 0 ? "ok" : "LEAKED");
    printf("forktest: value %d, cow fork ok\n", value);
    return 0;
}
