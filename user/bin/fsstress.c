/* /bin/fsstress [ops] [seed] (Day 26): FAT robustness test on /disk.
 * 1. Random create/truncate/write-at/append/unlink/rename/read on NF files in
 *    /disk/stress, checked against a RAM model after every read and at the end.
 * 2. Full disk: one file is written until write() fails with -ENOSPC; the partial
 *    size must be readable, df must show (almost) no free clusters, and unlinking it
 *    must give every cluster back.
 * 3. Everything is removed and the free-cluster count must equal the start value.
 * The host then runs `fatimg fsck` over the image (tests/run_tests.sh). */
#include "tern.h"

#define NF 10
#define MAXSZ 20000
static unsigned char *model[NF];
static long msize[NF];          /* -1 = file does not exist */
static unsigned char tmp[MAXSZ + 4096];
static unsigned long rng;
static unsigned rnd(void) { rng = rng * 6364136223846793005ul + 1442695040888963407ul; return (unsigned)(rng >> 33); }
static int bad;

static void path(int i, char *b) { snprintf(b, 64, "/disk/stress/File Number %d.dat", i); }
static uint64_t freeclus(void) { struct tern_statfs s; statfs("/disk", &s); return s.bfree; }

static void fail(const char *what, int i, long v) {
    if (bad++ < 5) printf("fsstress: MISMATCH %s file %d (%ld)\n", what, i, v);
}

static void verify(int i) {
    char p[64]; path(i, p);
    struct tern_stat st;
    int rc = stat(p, &st);
    if (msize[i] < 0) { if (rc != -ENOENT) fail("exists", i, rc); return; }
    if (rc) { fail("stat", i, rc); return; }
    if ((long)st.size != msize[i]) { fail("size", i, (long)st.size); return; }
    int fd = open(p, O_RDONLY);
    long n = read(fd, tmp, sizeof tmp);
    close(fd);
    if (n != msize[i] || memcmp(tmp, model[i], (size_t)n)) fail("data", i, n);
}

static void fill(unsigned char *b, long n) { for (long k = 0; k < n; k++) b[k] = (unsigned char)rnd(); }

int main(int argc, char **argv) {
    int ops = argc > 1 ? atoi(argv[1]) : 600;
    rng = argc > 2 ? (unsigned long)atoi(argv[2]) : 26;
    uint64_t f0 = freeclus();      /* before mkdir: the directory needs a cluster */
    mkdir("/disk/stress");
    for (int i = 0; i < NF; i++) { model[i] = malloc(MAXSZ); msize[i] = -1; }
    int cnt[7] = {0};
    char p[64], q[64];
    for (int op = 0; op < ops; op++) {
        int i = (int)(rnd() % NF), kind = (int)(rnd() % 7);
        path(i, p);
        if (kind == 0) {                    /* create / truncate with new contents */
            long n = rnd() % MAXSZ;
            fill(model[i], n);
            int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC);
            if (fd < 0 || write_all(fd, model[i], (size_t)n) != n) fail("create", i, fd);
            close(fd); msize[i] = n;
        } else if (kind == 1 && msize[i] >= 0) {   /* write at offset (may extend / make a hole) */
            long off = rnd() % (msize[i] + 2000), n = rnd() % 3000;
            if (off + n > MAXSZ) n = MAXSZ - off;
            if (n < 0) n = 0;
            if (off > msize[i]) memset(model[i] + msize[i], 0, (size_t)(off - msize[i]));
            fill(model[i] + off, n);
            int fd = open(p, O_WRONLY);
            lseek(fd, off, SEEK_SET);
            if (write_all(fd, model[i] + off, (size_t)n) != n) fail("pwrite", i, n);
            close(fd);
            if (off + n > msize[i] && n > 0) msize[i] = off + n;
            if (off > msize[i]) { /* zero-length write past EOF: no change */ }
        } else if (kind == 2 && msize[i] >= 0) {   /* append */
            long n = rnd() % 2500;
            if (msize[i] + n > MAXSZ) n = MAXSZ - msize[i];
            fill(model[i] + msize[i], n);
            int fd = open(p, O_WRONLY | O_APPEND);
            if (write_all(fd, model[i] + msize[i], (size_t)n) != n) fail("append", i, n);
            close(fd); msize[i] += n;
        } else if (kind == 3) {             /* unlink */
            int rc = unlink(p);
            if ((msize[i] < 0) != (rc == -ENOENT) || (msize[i] >= 0 && rc)) fail("unlink", i, rc);
            msize[i] = -1;
        } else if (kind == 4 && msize[i] >= 0) {   /* rename over another slot */
            int j = (int)(rnd() % NF);
            if (j == i) continue;
            path(j, q);
            int rc = rename(p, q);
            if (rc) { fail("rename", i, rc); continue; }
            unsigned char *t = model[j]; model[j] = model[i]; model[i] = t;
            msize[j] = msize[i]; msize[i] = -1;
        } else {
            verify(i);
            kind = 5;
        }
        cnt[kind]++;
    }
    for (int i = 0; i < NF; i++) verify(i);
    sync();
    printf("fsstress: %d ops (create %d, write %d, append %d, unlink %d, rename %d, check %d), %d mismatches\n",
           ops, cnt[0], cnt[1], cnt[2], cnt[3], cnt[4], cnt[5], bad);

    /* full disk */
    uint64_t before = freeclus();
    int fd = open("/disk/stress/filler.bin", O_WRONLY | O_CREAT | O_TRUNC);
    long total = 0, rc = 0;
    memset(tmp, 0xAB, 4096);
    while ((rc = write(fd, tmp, 4096)) > 0) total += rc;
    close(fd);
    uint64_t atfull = freeclus();
    struct tern_stat st;
    stat("/disk/stress/filler.bin", &st);
    fd = open("/disk/stress/x.txt", O_WRONLY | O_CREAT | O_TRUNC);
    long rc2 = fd >= 0 ? write(fd, "x", 1) : fd;
    if (fd >= 0) close(fd);
    unlink("/disk/stress/x.txt");
    printf("fsstress: disk full after %ld KiB -> write %ld (ENOSPC %s), size %s, %lu clusters free, next write %ld\n",
           total / 1024, rc, rc == -ENOSPC ? "ok" : "BAD", (long)st.size == total ? "ok" : "BAD",
           (unsigned long)atfull, rc2);
    unlink("/disk/stress/filler.bin");
    uint64_t after = freeclus();
    printf("fsstress: after unlink %s (%lu -> %lu)\n", after == before ? "all clusters back" : "LEAK",
           (unsigned long)before, (unsigned long)after);

    for (int i = 0; i < NF; i++) { path(i, p); unlink(p); }
    int rr = unlink("/disk/stress");
    sync();
    uint64_t end = freeclus();
    int ok = !bad && rc == -ENOSPC && after == before && end == f0 && rr == 0;
    printf("fsstress: cleanup -> free clusters %s (%lu, start %lu); %s\n", end == f0 ? "equal" : "DIFFER",
           (unsigned long)end, (unsigned long)f0, ok ? "fs stress ok" : "FAILED");
    return ok ? 0 : 1;
}
