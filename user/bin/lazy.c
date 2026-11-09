/* /bin/lazy (Day 20): demand-paging check. Grows the heap by 4 MiB with brk and shows
 * that no frames are used until pages are touched, then recurses ~512 KiB deep so the
 * stack grows on demand. `lazy guard` recurses without end and must be killed at the
 * stack guard page. */
#include "tern.h"

static unsigned long pages(void) {
    struct tern_sysinfo si;
    sysinfo(&si);
    return (unsigned long)si.user_pages;
}

static unsigned long frames_free(void) {
    struct tern_sysinfo si;
    sysinfo(&si);
    return (unsigned long)si.frames_free;
}

static unsigned long bottom_pages;

/* 4 KiB of locals per level, kept live so the compiler cannot drop them */
__attribute__((noinline)) static unsigned long deep(int n) {
    volatile unsigned char buf[4000];
    buf[0] = (unsigned char)n;
    for (unsigned i = 0; i < sizeof buf; i += 512) buf[i] = (unsigned char)(n + i);
    if (n == 0) { bottom_pages = pages(); return buf[0]; }
    return deep(n - 1) + buf[0] + buf[(unsigned)n * 31 % sizeof buf];
}

static unsigned long forever(unsigned long n) {
    volatile unsigned char buf[4000];
    buf[0] = (unsigned char)n;
    if (n == ~0ul) return buf[0];          /* never true; keeps clang quiet */
    return forever(n + 1) + buf[0];
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "guard") == 0) {
        printf("lazy: recursing until the guard page\n");
        return (int)forever(0);
    }
    const unsigned long size = 4ul << 20;
    unsigned long p0 = pages(), f0 = frames_free();
    uintptr_t base = (uintptr_t)brk(0);
    if ((uintptr_t)brk(base + size) != base + size) { printf("lazy: brk failed\n"); return 1; }
    unsigned long p1 = pages(), f1 = frames_free();
    printf("lazy: brk +4096 KiB -> %lu new pages mapped, %lu frames used\n", p1 - p0, f0 - f1);
    unsigned char *h = (unsigned char *)base;
    for (unsigned long i = 0; i < size; i += 4096) {
        if (h[i]) { printf("lazy: page not zero\n"); return 1; }
        h[i] = (unsigned char)(i >> 12);
    }
    unsigned long p2 = pages();
    int bad = 0;
    for (unsigned long i = 0; i < size; i += 4096) bad += h[i] != (unsigned char)(i >> 12);
    printf("lazy: touched 1024 heap pages -> %lu new pages mapped, %d bad\n", p2 - p1, bad);
    /* a syscall reading from a fresh lazy page (kernel faults it in, no EFAULT) */
    brk(base + size + 8192);
    long w = write(1, (const void *)(base + size + 4096), 0);
    char *msg = (char *)(base + size + 100);
    memcpy(msg, "lazy: syscall buffer in lazy page ok\n", 37);
    w = write(1, msg, 37);
    if (w != 37) printf("lazy: write gave %ld\n", w);
    unsigned long s0 = pages();
    deep(128);
    unsigned long s1 = bottom_pages;
    printf("lazy: 128-deep recursion -> stack grew by %lu pages\n", s1 - s0);
    unsigned long fb = frames_free();
    brk(base);
    unsigned long fa = frames_free();
    printf("lazy: brk shrink freed %lu frames\n", fa - fb);
    return bad ? 1 : 0;
}
