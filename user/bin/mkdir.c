/* /bin/mkdir DIR...: create directories. */
#include "tern.h"

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: mkdir DIR...\n"); return 2; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int r = mkdir(argv[i]);
        if (r == -EEXIST) printf("mkdir: %s: already exists\n", argv[i]);
        else if (r == -ENOENT) printf("mkdir: %s: parent not found\n", argv[i]);
        else if (r) printf("mkdir: %s: error %d\n", argv[i], r);
        if (r) rc = 1;
    }
    return rc;
}
