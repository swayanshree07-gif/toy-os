/* /bin/yes [WORD]: print WORD (default "y") forever, until the output goes away
 * (a closed pipe gives -EPIPE) or Ctrl-C. Exit status 0 on EPIPE. */
#include "tern.h"

int main(int argc, char **argv) {
    const char *w = argc > 1 ? argv[1] : "y";
    char buf[512];
    size_t len = strlen(w), n = 0;
    if (len > 64) len = 64;
    while (n + len + 1 <= sizeof buf) {          /* fill a buffer with copies */
        memcpy(buf + n, w, len);
        buf[n + len] = '\n';
        n += len + 1;
    }
    for (;;) {
        long r = write_all(1, buf, n);
        if (r < 0) return r == -EPIPE ? 0 : 1;
    }
}
