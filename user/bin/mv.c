/* /bin/mv SRC DST: rename/move a file or directory. If DST is an existing directory,
 * SRC is moved into it under its own name. */
#include "tern.h"

static const char *basename_of(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; q++) if (*q == '/' && q[1]) b = q + 1;
    return b;
}

static const char *err_name(int e) {
    switch (-e) {
    case ENOENT: return "not found";
    case EINVAL: return "can't move a directory into itself";
    case ENOTEMPTY: return "directory not empty";
    case EISDIR: return "is a directory";
    case ENOTDIR: return "not a directory";
    case EBUSY: return "busy";
    default: return "error";
    }
}

int main(int argc, char **argv) {
    if (argc != 3) { printf("usage: mv SRC DST\n"); return 1; }
    char dst[256];
    struct tern_stat st;
    if (stat(argv[2], &st) == 0 && st.type == TERN_FT_DIR) {
        size_t l = strlen(argv[2]);
        snprintf(dst, sizeof dst, "%s%s%s", argv[2], l && argv[2][l - 1] == '/' ? "" : "/",
                 basename_of(argv[1]));
    } else {
        snprintf(dst, sizeof dst, "%s", argv[2]);
    }
    int rc = rename(argv[1], dst);
    if (rc) { printf("mv: %s -> %s: %s\n", argv[1], dst, err_name(rc)); return 1; }
    return 0;
}
