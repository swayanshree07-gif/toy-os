/* /bin/tee [-a] FILE: copy standard input to standard output and FILE (-a appends). */
#include "tern.h"

int main(int argc, char **argv) {
    int i = 1, flags = O_WRONLY | O_CREAT | O_TRUNC;
    if (i < argc && !strcmp(argv[i], "-a")) { flags = O_WRONLY | O_CREAT | O_APPEND; i++; }
    if (i >= argc) { printf("usage: tee [-a] FILE\n"); return 1; }
    int fd = open(argv[i], flags);
    if (fd < 0) { printf("tee: %s: can't open (%d)\n", argv[i], fd); return 1; }
    char buf[512];
    long n;
    while ((n = read(0, buf, sizeof buf)) > 0) {
        write_all(fd, buf, (size_t)n);
        if (write_all(1, buf, (size_t)n) < 0) break;
    }
    close(fd);
    return 0;
}
