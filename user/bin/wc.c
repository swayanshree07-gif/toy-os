/* /bin/wc [FILE...]: count lines, words and bytes (standard input without arguments). */
#include "tern.h"

static int count(int fd, const char *name) {
    char buf[256];
    unsigned long lines = 0, words = 0, bytes = 0;
    bool in_word = false;
    long n;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        for (long i = 0; i < n; i++) {
            char c = buf[i];
            bytes++;
            if (c == '\n') lines++;
            bool space = c == ' ' || c == '\t' || c == '\n';
            if (!space && !in_word) words++;
            in_word = !space;
        }
    }
    if (n < 0) { printf("wc: %s: read error %ld\n", name, n); return 1; }
    printf("%lu %lu %lu%s%s\n", lines, words, bytes, *name ? " " : "", name);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) return count(0, "");
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) { printf("wc: %s: not found\n", argv[i]); rc = 1; continue; }
        rc |= count(fd, argv[i]);
        close(fd);
    }
    return rc;
}
