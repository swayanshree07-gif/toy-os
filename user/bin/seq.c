/* /bin/seq [FIRST] LAST: print the numbers FIRST (default 1) .. LAST, one per line. */
#include "tern.h"

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: seq [FIRST] LAST\n"); return 1; }
    long first = argc > 2 ? atoi(argv[1]) : 1, last = atoi(argv[argc > 2 ? 2 : 1]);
    char buf[512];
    size_t n = 0;
    for (long v = first; v <= last; v++) {
        n += (size_t)snprintf(buf + n, sizeof buf - n, "%ld\n", v);
        if (n > sizeof buf - 24 || v == last) {
            if (write_all(1, buf, n) < 0) return 1;
            n = 0;
        }
    }
    return 0;
}
