/* /bin/hexdump [-n LEN] [-s OFF] [file]: canonical hex + ASCII dump (like hexdump -C).
 * Reads stdin without a file. Repeated identical lines are folded into "*". */
#include "tern.h"

static void line(uint64_t off, const unsigned char *b, int n) {
    char out[96];
    int k = snprintf(out, sizeof out, "%08lx ", (unsigned long)off);
    for (int i = 0; i < 16; i++) {
        if (i == 8) out[k++] = ' ';
        if (i < n) k += snprintf(out + k, sizeof out - k, " %02x", b[i]);
        else { out[k++] = ' '; out[k++] = ' '; out[k++] = ' '; }
    }
    k += snprintf(out + k, sizeof out - k, "  |");
    for (int i = 0; i < n; i++) out[k++] = b[i] >= 32 && b[i] < 127 ? (char)b[i] : '.';
    out[k++] = '|'; out[k++] = '\n';
    write_all(1, out, (size_t)k);
}

int main(int argc, char **argv) {
    long limit = -1, skip = 0;
    const char *file = NULL;
    for (int i = 1; i < argc; i++) {             /* options may come after the file */
        if (!strcmp(argv[i], "-n") && i + 1 < argc) limit = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) skip = atoi(argv[++i]);
        else if (argv[i][0] == '-' || file) {
            printf("usage: hexdump [-n LEN] [-s OFF] [file]\n");
            return 2;
        } else file = argv[i];
    }
    int fd = 0;
    if (file) {
        fd = open(file, O_RDONLY);
        if (fd < 0) { printf("hexdump: %s: cannot open (%d)\n", file, fd); return 1; }
    }
    uint64_t off = 0;
    if (skip > 0) {
        if (fd != 0 && lseek(fd, skip, SEEK_SET) == skip) off = (uint64_t)skip;
        else {                                   /* pipes: read and drop */
            char junk[256];
            while (off < (uint64_t)skip) {
                long want = skip - (long)off > 256 ? 256 : skip - (long)off;
                long r = read(fd, junk, (size_t)want);
                if (r <= 0) break;
                off += (uint64_t)r;
            }
        }
    }
    unsigned char cur[16], prev[16];
    int have = 0, folded = 0, any = 0;
    long left = limit;
    for (;;) {
        int fill = 0;
        while (fill < 16 && left != 0) {
            long want = 16 - fill;
            if (left > 0 && want > left) want = left;
            long r = read(fd, cur + fill, (size_t)want);
            if (r <= 0) break;
            fill += (int)r;
            if (left > 0) left -= r;
        }
        if (fill == 0) break;
        if (fill == 16 && have && !memcmp(cur, prev, 16)) {
            if (!folded) { write_all(1, "*\n", 2); folded = 1; }
        } else {
            line(off, cur, fill);
            folded = 0;
        }
        memcpy(prev, cur, 16);
        have = fill == 16;
        off += (uint64_t)fill;
        any = 1;
        if (fill < 16) break;
    }
    if (any || skip) printf("%08lx\n", (unsigned long)off);
    if (fd) close(fd);
    return 0;
}
