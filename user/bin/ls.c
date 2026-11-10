/* /bin/ls [-l] [dirs...]: list directory entries. Directories end in '/', devices in '*',
 * files show their size. -l: one line per entry "T INODE SIZE NAME" (T = d, -, c;
 * directories show their number of entries as size). Default: current directory. */
#include "tern.h"

static bool longfmt;

static void long_line(const char *dir, const char *name) {
    char path[256];
    size_t dl = strlen(dir);
    snprintf(path, sizeof path, "%s%s%s", dir, dl && dir[dl - 1] == '/' ? "" : "/", name);
    struct tern_stat st;
    if (stat(path, &st)) { printf("? %6s %8s  %s\n", "?", "?", name); return; }
    char t = st.type == TERN_FT_DIR ? 'd' : st.type == TERN_FT_DEV ? 'c' : '-';
    uint64_t size = st.type == TERN_FT_DIR ? st.nlink : st.size;
    printf("%c %6lu %8lu  %s%s\n", t, (unsigned long)st.ino, (unsigned long)size, name,
           st.type == TERN_FT_DIR ? "/" : "");
}

static int list(const char *path, bool header) {
    struct tern_stat st;
    int rc = stat(path, &st);
    if (rc) { printf("ls: %s: not found\n", path); return 1; }
    if (st.type != TERN_FT_DIR) {
        if (longfmt) printf("%c %6lu %8lu  %s\n", st.type == TERN_FT_DEV ? 'c' : '-',
                            (unsigned long)st.ino, (unsigned long)st.size, path);
        else printf("%s\n", path);
        return 0;
    }
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) { printf("ls: %s: cannot open (%d)\n", path, fd); return 1; }
    if (header) printf("%s:\n", path);
    struct tern_dirent d;
    unsigned n = 0;
    uint64_t bytes = 0;
    while (readdir(fd, &d) == 1) {
        n++;
        if (d.type == TERN_FT_FILE) bytes += d.size;
        if (longfmt) long_line(path, d.name);
        else if (d.type == TERN_FT_DIR) printf("  %s/\n", d.name);
        else if (d.type == TERN_FT_DEV) printf("  %s*\n", d.name);
        else printf("  %-24s %8lu\n", d.name, (unsigned long)d.size);
    }
    if (longfmt) printf("total %u entries, %lu bytes\n", n, (unsigned long)bytes);
    close(fd);
    return 0;
}

int main(int argc, char **argv) {
    int i = 1;
    if (i < argc && !strcmp(argv[i], "-l")) { longfmt = true; i++; }
    if (i >= argc) return list(".", false);
    int rc = 0;
    for (int k = i; k < argc; k++) rc |= list(argv[k], argc - i > 1);
    return rc;
}
