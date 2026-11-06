/* /bin/kill PID...: ask processes to exit (status 137). */
#include "tern.h"

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: kill PID...\n"); return 2; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int r = kill(atoi(argv[i]));
        if (r) { printf("kill: %s: no such process\n", argv[i]); rc = 1; }
    }
    return rc;
}
