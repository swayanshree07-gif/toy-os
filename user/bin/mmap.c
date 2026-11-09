/* /bin/mmap (Day 21): anonymous mmap/munmap check. Maps 1 MiB lazily, touches it, punches
 * a hole with munmap (splitting the region), reuses the hole, then frees everything and
 * compares frame counts. `mmap after` touches memory after munmap and must be killed;
 * `mmap ro` writes to a read-only mapping and must be killed. */
#include "tern.h"

static struct tern_sysinfo si;
static unsigned long pages(void)  { sysinfo(&si); return (unsigned long)si.user_pages; }
static unsigned long frames(void) { sysinfo(&si); return (unsigned long)si.frames_free; }

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "after") == 0) {
        volatile int *q = (volatile int *)mmap(4096, PROT_READ | PROT_WRITE);
        *q = 42;
        printf("mmap: wrote %d, unmapping and touching again\n", *q);
        munmap((void *)q, 4096);
        *q = 43;
        printf("mmap: still alive?!\n");
        return 1;
    }
    if (argc > 1 && strcmp(argv[1], "ro") == 0) {
        volatile char *r = (volatile char *)mmap(8192, PROT_READ);
        printf("mmap: read-only page reads %d, writing\n", r[100]);
        r[100] = 1;
        printf("mmap: still alive?!\n");
        return 1;
    }
    const unsigned long size = 1ul << 20, np = size / 4096;
    /* warm-up: creates the page tables for the mmap area, which stay afterwards */
    long w = mmap(4096, PROT_READ | PROT_WRITE);
    *(volatile char *)w = 1;
    munmap((void *)w, 4096);
    unsigned long f0 = frames(), p0 = pages();
    long a = mmap(size, PROT_READ | PROT_WRITE);
    if (a < 0) { printf("mmap: failed %ld\n", a); return 1; }
    unsigned long p1 = pages();
    printf("mmap: 1024 KiB at %lx -> %lu pages mapped\n", (unsigned long)a, p1 - p0);
    unsigned char *m = (unsigned char *)a;
    int bad = 0;
    for (unsigned long i = 0; i < size; i += 4096) { bad += m[i] != 0; m[i] = (unsigned char)(i >> 12); }
    for (unsigned long i = 0; i < size; i += 4096) bad += m[i] != (unsigned char)(i >> 12);
    printf("mmap: touched %lu pages -> %lu mapped, %d bad\n", np, pages() - p1, bad);
    /* a syscall writing straight into a fresh lazy mmap page */
    long b = mmap(4096, PROT_READ | PROT_WRITE);
    int fd = open("/etc/motd", 0);
    long n = fd >= 0 ? read(fd, (void *)b, 16) : -1;
    if (fd >= 0) close(fd);
    printf("mmap: read() into lazy mmap page -> %ld\n", n);
    unsigned long q0 = pages();
    if (munmap(m + 64 * 4096, 64 * 4096)) { printf("mmap: munmap failed\n"); return 1; }
    printf("mmap: munmap middle 64 pages -> %lu pages freed\n", q0 - pages());
    for (unsigned long i = 0; i < size; i += 4096)
        if (i < 64 * 4096 || i >= 128 * 4096) bad += m[i] != (unsigned char)(i >> 12);
    printf("mmap: outer parts intact, %d bad\n", bad);
    long c = mmap(4096, PROT_READ | PROT_WRITE);
    printf("mmap: next small map %s the hole\n",
           c >= a + 64 * 4096 && c < a + 128 * 4096 ? "reuses" : "does not reuse");
    printf("mmap: bad args -> %d %d %ld\n", munmap((void *)(a + 1), 4096), munmap(0, 4096),
           mmap(0, PROT_READ));
    munmap(m, size);
    munmap((void *)b, 4096);
    munmap((void *)c, 4096);
    long d = (long)f0 - (long)frames();
    printf("mmap: all unmapped -> %lu pages, frames %s (%ld)\n", pages() - p0,
           d == 0 ? "back to start" : "LEAKED", d);
    return bad ? 1 : 0;
}
