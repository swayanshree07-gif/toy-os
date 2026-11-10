/* /bin/diskfault (Day 27): fault isolation during disk I/O on /disk.
 * 1. A child writes 4 KiB chunks to a FAT file in a loop; the parent kills it while it
 *    is writing. The file must hold a whole number of intact chunks.
 * 2. A child opens three files, writes them and then dereferences NULL with every fd
 *    still open: only the child dies (exit 128+14), the files are complete and can be
 *    unlinked (the kernel closed the child's fds).
 * 3. write() from a buffer whose second page was unmapped: must fail cleanly (or write
 *    only the mapped part) and never kill the process or the kernel.
 * 4. Everything removed: free clusters equal the start value.
 * The shell must keep running afterwards and the host runs `fatimg fsck` on the image. */
#include "tern.h"

#define CH 4096
static unsigned char buf[CH];
static int bad;
static uint64_t freeclus(void) { struct tern_statfs s; statfs("/disk", &s); return s.bfree; }
static void fill(int k) { for (int i = 0; i < CH; i++) buf[i] = (unsigned char)(k * 31 + i); }
#define CHECK(c, ...) do { if (!(c)) { bad++; printf("diskfault: FAIL " __VA_ARGS__); puts(""); } } while (0)

static long fsize(const char *p) { struct tern_stat st; return stat(p, &st) ? -1 : (long)st.size; }

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    uint64_t f0 = freeclus();
    mkdir("/disk/fault");

    /* 1. kill during writes */
    int pid = fork();
    if (pid == 0) {
        int fd = open("/disk/fault/Killed Writer.dat", O_WRONLY | O_CREAT | O_TRUNC);
        for (int k = 0; k < 400; k++) { fill(k); write_all(fd, buf, CH); }
        exit(9);                       /* not reached in time (1.6 MiB > disk) */
    }
    sleep_ms(40);
    int st = -1;
    CHECK(kill(pid) == 0, "kill");
    waitpid(pid, &st, 0);
    long sz = fsize("/disk/fault/Killed Writer.dat");
    int chunks = 0, intact = 1;
    if (sz > 0) {
        int fd = open("/disk/fault/Killed Writer.dat", O_RDONLY);
        static unsigned char rb[CH];
        while (read(fd, rb, CH) == CH) {
            fill(chunks);
            if (memcmp(rb, buf, CH)) intact = 0;
            chunks++;
        }
        close(fd);
    }
    CHECK(st == TERN_EXIT_KILLED && sz % CH == 0 && sz / CH == chunks && intact,
          "killed writer st %d size %ld", st, sz);
    printf("diskfault: writer killed during I/O (status %d) -> %d whole chunks, data %s\n",
           st, chunks, intact ? "ok" : "BAD");

    /* 2. segfault with open fds */
    pid = fork();
    if (pid == 0) {
        int fds[3];
        char p[48];
        for (int i = 0; i < 3; i++) {
            snprintf(p, sizeof p, "/disk/fault/Open File %d.txt", i);
            fds[i] = open(p, O_WRONLY | O_CREAT | O_TRUNC);
            fill(i); write_all(fds[i], buf, CH + 0 * i);
        }
        volatile int *np = (volatile int *)(uintptr_t)(fds[0] < 0);   /* 0 */
        *np = 1;
        exit(1);
    }
    waitpid(pid, &st, 0);
    int okf = 0;
    for (int i = 0; i < 3; i++) {
        char p[48]; snprintf(p, sizeof p, "/disk/fault/Open File %d.txt", i);
        if (fsize(p) == CH && unlink(p) == 0) okf++;
    }
    CHECK(st == 128 + 14 && okf == 3, "segv child st %d files %d", st, okf);
    printf("diskfault: child faulted with 3 open files (status %d) -> %d/3 complete, unlinked\n", st, okf);

    /* 3. write from a half-unmapped buffer */
    unsigned char *m = (unsigned char *)mmap(2 * CH, PROT_READ | PROT_WRITE);
    memset(m, 'x', 2 * CH);
    munmap(m + CH, CH);
    int fd = open("/disk/fault/efault.dat", O_WRONLY | O_CREAT | O_TRUNC);
    long r = write(fd, m, 2 * CH);
    close(fd);
    long esz = fsize("/disk/fault/efault.dat");
    CHECK((r == -EFAULT && esz == 0) || (r == CH && esz == CH), "efault r %ld size %ld", r, esz);
    printf("diskfault: write from half-unmapped buffer -> %ld, file size %ld, still alive\n", r, esz);

    unlink("/disk/fault/efault.dat");
    unlink("/disk/fault/Killed Writer.dat");
    unlink("/disk/fault");
    uint64_t f1 = freeclus();
    CHECK(f1 == f0, "clusters %lu vs %lu", (unsigned long)f1, (unsigned long)f0);
    printf("diskfault: cleanup -> free clusters %s (%lu, start %lu); %s\n", f1 == f0 ? "equal" : "DIFFER",
           (unsigned long)f1, (unsigned long)f0, bad ? "FAILED" : "fault isolation ok");
    return bad ? 1 : 0;
}
