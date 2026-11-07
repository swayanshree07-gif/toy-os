/* /bin/head [-n N] [FILE]: the first N (default 10) lines of FILE or standard input. */
#include "tern.h"

int main(int argc, char **argv) {
    long want = 10;
    int i = 1;
    if (i + 1 < argc && !strcmp(argv[i], "-n")) { want = atoi(argv[i + 1]); i += 2; }
    int fd = 0;
    if (i < argc) {
        fd = open(argv[i], O_RDONLY);
        if (fd < 0) { printf("head: %s: not found\n", argv[i]); return 1; }
    }
    char buf[256];
    long lines = 0, n;
    while (lines < want && (n = read(fd, buf, sizeof buf)) > 0) {
        long k = 0;
        while (k < n && lines < want) if (buf[k++] == '\n') lines++;
        if (write_all(1, buf, (size_t)k) < 0) return 1;
    }
    return 0;      /* exiting closes our end of a pipe: the writer sees EPIPE */
}
