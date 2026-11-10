/* /bin/df [paths...]: file system usage (statfs). Without arguments shows / and, if it
 * exists, /disk. Sizes in KiB. */
#include "tern.h"

static int show(const char *path) {
    struct tern_statfs s;
    int rc = statfs(path, &s);
    if (rc) { printf("df: %s: error %d\n", path, rc); return 1; }
    uint64_t total = s.blocks * s.bsize / 1024, avail = s.bfree * s.bsize / 1024;
    uint64_t used = total - avail, pct = total ? (used * 100 + total - 1) / total : 0;
    printf("%-6s %10lu %10lu %10lu %3lu%%  %s\n", s.fstype, (unsigned long)total,
           (unsigned long)used, (unsigned long)avail, (unsigned long)pct, s.mount);
    return 0;
}

int main(int argc, char **argv) {
    printf("Type    Size(KiB)  Used(KiB) Avail(KiB) Use%%  Mounted on\n");
    if (argc < 2) {
        struct tern_stat st;
        int rc = show("/");
        if (stat("/disk", &st) == 0) rc |= show("/disk");
        return rc;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) rc |= show(argv[i]);
    return rc;
}
