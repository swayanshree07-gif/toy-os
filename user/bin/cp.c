/* /bin/cp SRC DST: copy a file. If DST is an existing directory, the copy goes into it
 * under SRC's name. */
#include "tern.h"

int main(int argc, char **argv) {
    if (argc != 3) { printf("usage: cp SRC DST\n"); return 1; }
    struct tern_stat st;
    if (stat(argv[1], &st) == 0 && st.type == TERN_FT_DIR) {
        printf("cp: %s: is a directory\n", argv[1]);
        return 1;
    }
    char dst[256];
    if (stat(argv[2], &st) == 0 && st.type == TERN_FT_DIR) {
        const char *b = argv[1];
        for (const char *q = argv[1]; *q; q++) if (*q == '/' && q[1]) b = q + 1;
        size_t l = strlen(argv[2]);
        snprintf(dst, sizeof dst, "%s%s%s", argv[2], l && argv[2][l - 1] == '/' ? "" : "/", b);
    } else {
        snprintf(dst, sizeof dst, "%s", argv[2]);
    }
    int in = open(argv[1], O_RDONLY);
    if (in < 0) { printf("cp: %s: not found\n", argv[1]); return 1; }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) { printf("cp: %s: can't create (%d)\n", dst, out); close(in); return 1; }
    char buf[1024];
    long n;
    int rc = 0;
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write_all(out, buf, (size_t)n) < 0) { printf("cp: %s: write error\n", dst); rc = 1; break; }
    if (n < 0) { printf("cp: %s: read error\n", argv[1]); rc = 1; }
    close(in);
    close(out);
    return rc;
}
