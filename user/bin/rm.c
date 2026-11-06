/* /bin/rm PATH...: remove files or empty directories. */
#include "tern.h"

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: rm PATH...\n"); return 2; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int r = unlink(argv[i]);
        if (r == -ENOENT) printf("rm: %s: not found\n", argv[i]);
        else if (r == -ENOTEMPTY) printf("rm: %s: directory not empty\n", argv[i]);
        else if (r) printf("rm: %s: error %d\n", argv[i], r);
        if (r) rc = 1;
    }
    return rc;
}
