/* /bin/grep [-v] [-c] TEXT [FILE...]: lines containing TEXT (plain substring).
 * -v: lines NOT containing it; -c: print only the count. Exit 0 if a line matched,
 * 1 if none, 2 on errors. */
#include "tern.h"

static bool invert, count_only;
static long matches;

static bool contains(const char *line, const char *pat) {
    size_t pl = strlen(pat);
    for (const char *p = line; *p || pl == 0; p++) {
        if (!strncmp(p, pat, pl)) return true;
        if (!*p) break;
    }
    return false;
}

static void line_done(char *line, size_t len, const char *pat) {
    line[len] = 0;
    if (contains(line, pat) != invert) {
        matches++;
        if (!count_only) { line[len] = '\n'; write_all(1, line, len + 1); }
    }
}

static int scan(int fd, const char *pat) {
    static char line[1024];
    size_t len = 0;
    char buf[256];
    long n;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        for (long i = 0; i < n; i++) {
            if (buf[i] == '\n') { line_done(line, len, pat); len = 0; }
            else if (len < sizeof line - 2) line[len++] = buf[i];
        }
    }
    if (len) line_done(line, len, pat);
    return n < 0 ? 2 : 0;
}

int main(int argc, char **argv) {
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-v")) invert = true;
        else if (!strcmp(argv[i], "-c")) count_only = true;
        else break;
    }
    if (i >= argc) { printf("usage: grep [-v] [-c] TEXT [FILE...]\n"); return 2; }
    const char *pat = argv[i++];
    int rc = 0;
    if (i == argc) rc = scan(0, pat);
    for (; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) { printf("grep: %s: not found\n", argv[i]); rc = 2; continue; }
        if (scan(fd, pat)) rc = 2;
        close(fd);
    }
    if (count_only) printf("%ld\n", matches);
    return rc ? rc : matches ? 0 : 1;
}
