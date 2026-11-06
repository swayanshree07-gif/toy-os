/* /bin/cat [files...]: copy files (or the console input) to the console. */
#include "tern.h"

static int copy(int fd, const char *name) {
    char buf[512];
    long n;
    while ((n = read(fd, buf, sizeof buf)) > 0) write(1, buf, (size_t)n);
    if (n < 0) { printf("cat: %s: read error %ld\n", name, n); return 1; }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) return copy(0, "stdin");
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) { printf("cat: %s: cannot open (%d)\n", argv[i], fd); rc = 1; continue; }
        rc |= copy(fd, argv[i]);
        close(fd);
    }
    return rc;
}
