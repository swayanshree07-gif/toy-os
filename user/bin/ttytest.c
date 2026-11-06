/* /bin/ttytest: reads the console through the TTY line discipline (plan Day 22).
 * The test harness types:  "abx<BS>c<Enter>", "zz<Ctrl-U>ok go<Enter>", "<Ctrl-D>".
 * Expected reads: "abc\n", then "ok go\n" in two parts (small buffer), then EOF. */
#include "tern.h"

static int check_no;
#define CHECK(c) do { check_no++; if (!(c)) { \
    printf("ttytest: check %d failed: %s\n", check_no, #c); exit(check_no); } } while (0)

int main(void) {
    char a[64], b[4], c[64];
    printf("[TEST] tty: waiting for input\n");
    long n = read(0, a, sizeof a - 1);
    CHECK(n == 4 && !memcmp(a, "abc\n", 4));
    n = read(0, b, sizeof b);                    /* only 4 of "ok go\n" fit */
    CHECK(n == 4 && !memcmp(b, "ok g", 4));
    n = read(0, c, sizeof c);                    /* the rest of the same line */
    CHECK(n == 2 && !memcmp(c, "o\n", 2));
    n = read(0, c, sizeof c);                    /* Ctrl-D at the start of a line */
    CHECK(n == 0);
    CHECK(read(0, (void *)0xFFFFFFFF80100000ull, 8) == -EFAULT);
    a[3] = 0;
    printf("[TEST] tty read ok: '%s', 'ok go' in 2 reads, backspace, ^U, ^D eof\n", a);
    return 0;
}
