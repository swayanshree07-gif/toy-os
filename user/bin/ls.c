/* /bin/ls [dirs...]: list directory entries. Directories end in '/', devices in '*',
 * files show their size. Without arguments lists the current directory. */
#include "tern.h"

static int list(const char *path, bool header) {
    struct tern_stat st;
    int rc = stat(path, &st);
    if (rc) { printf("ls: %s: not found\n", path); return 1; }
    if (st.type != TERN_FT_DIR) { printf("%s\n", path); return 0; }
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) { printf("ls: %s: cannot open (%d)\n", path, fd); return 1; }
    if (header) printf("%s:\n", path);
    struct tern_dirent d;
    while (readdir(fd, &d) == 1) {
        if (d.type == TERN_FT_DIR) printf("  %s/\n", d.name);
        else if (d.type == TERN_FT_DEV) printf("  %s*\n", d.name);
        else printf("  %-24s %8lu\n", d.name, (unsigned long)d.size);
    }
    close(fd);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) return list(".", false);
    int rc = 0;
    for (int i = 1; i < argc; i++) rc |= list(argv[i], argc > 2);
    return rc;
}
