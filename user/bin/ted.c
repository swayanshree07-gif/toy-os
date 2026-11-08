/* /bin/ted [FILE]: a small ed-style line editor (Day 18, own code).
 *
 * Commands are read one per line from standard input (the console or a redirected
 * file). An optional address range comes first:
 *   N  line N     .  current line     $  last line     ,  all lines (1,$)
 *   A,B  lines A..B  (each of A and B may be N, . or $; "+N"/"-N" are relative to .)
 * Commands:
 *   a / i / c   append after / insert before / change the addressed line(s); then type
 *               text lines, ended by a line with a single "."
 *   d           delete lines            p   print lines         n   print with numbers
 *   =           print the number of lines
 *   w [FILE]    write (FILE becomes the default file name); prints the byte count
 *   q           quit (refuses once if there are unsaved changes)   Q   quit anyway
 *   (address only) make that line current and print it
 * Errors print "?" plus a short reason, like ed's help mode. */
#include "tern.h"

#define MAX_LINES 2048
#define MAX_LINE  512

static char *lines[MAX_LINES];
static int nlines, cur;          /* cur: 1-based current line, 0 when empty */
static bool dirty;
static char fname[128];

/* ---- buffered line input from fd 0 ---- */
static char ibuf[512];
static long ilen, ipos;
static bool ieof;

/* reads one line without the newline; false at end of input */
static bool getline0(char *out, size_t max) {
    size_t n = 0;
    for (;;) {
        if (ipos == ilen) {
            if (ieof) break;
            ilen = read(0, ibuf, sizeof ibuf); ipos = 0;
            if (ilen <= 0) { ilen = 0; ieof = true; break; }
        }
        char c = ibuf[ipos++];
        if (c == '\n') { out[n] = 0; return true; }
        if (c != '\r' && n + 1 < max) out[n++] = c;
    }
    out[n] = 0;
    return n > 0;
}

static char *dupstr(const char *s) {
    size_t n = strlen(s);
    char *p = malloc(n + 1);
    if (p) memcpy(p, s, n + 1);
    return p;
}

static bool insert_line(int at, const char *s) {   /* new line becomes number at+1 */
    if (nlines >= MAX_LINES) return false;
    char *p = dupstr(s);
    if (!p) return false;
    memmove(&lines[at + 1], &lines[at], (size_t)(nlines - at) * sizeof lines[0]);
    lines[at] = p;
    nlines++;
    return true;
}

static void delete_lines(int a, int b) {
    for (int i = a; i <= b; i++) free(lines[i - 1]);
    memmove(&lines[a - 1], &lines[b], (size_t)(nlines - b) * sizeof lines[0]);
    nlines -= b - a + 1;
}

static long load(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return fd;
    char buf[256], line[MAX_LINE];
    size_t ln = 0;
    long total = 0, n;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        total += n;
        for (long i = 0; i < n; i++) {
            if (buf[i] == '\n') { line[ln] = 0; insert_line(nlines, line); ln = 0; }
            else if (ln + 1 < MAX_LINE) line[ln++] = buf[i];
        }
    }
    if (ln) { line[ln] = 0; insert_line(nlines, line); }
    close(fd);
    return n < 0 ? n : total;
}

static long save(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return fd;
    long total = 0;
    for (int i = 0; i < nlines; i++) {
        size_t n = strlen(lines[i]);
        long r = write_all(fd, lines[i], n);
        if (r >= 0) r = write_all(fd, "\n", 1);
        if (r < 0) { close(fd); return r; }
        total += (long)n + 1;
    }
    close(fd);
    return total;
}

static void err(const char *why) { printf("? %s\n", why); }

/* parses one address at *pp; returns -1 if none, -2 on a bad one */
static int parse_addr(const char **pp) {
    const char *p = *pp;
    int base, v = 0;
    if (*p == '.') { base = cur; p++; }
    else if (*p == '$') { base = nlines; p++; }
    else if (*p >= '0' && *p <= '9') {
        while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
        *pp = p;
        return v;
    } else if (*p == '+' || *p == '-') base = cur;
    else return -1;
    while (*p == '+' || *p == '-') {
        int sign = *p++ == '+' ? 1 : -1, d = 0;
        bool any = false;
        while (*p >= '0' && *p <= '9') { d = d * 10 + (*p++ - '0'); any = true; }
        base += sign * (any ? d : 1);
    }
    *pp = p;
    return base;
}

static void print_range(int a, int b, bool num) {
    for (int i = a; i <= b; i++) {
        if (num) printf("%d\t%s\n", i, lines[i - 1]);
        else printf("%s\n", lines[i - 1]);
    }
    cur = b;
}

/* reads text lines until "." and inserts them after line `after` */
static void input_mode(int after) {
    char line[MAX_LINE];
    while (getline0(line, sizeof line) || !ieof) {
        if (line[0] == '.' && line[1] == 0) break;
        if (!insert_line(after, line)) { err("buffer full"); break; }
        after++;
        dirty = true;
    }
    cur = after;
}

int main(int argc, char **argv) {
    if (argc > 2) { puts("usage: ted [FILE]"); return 2; }
    if (argc == 2) {
        snprintf(fname, sizeof fname, "%s", argv[1]);
        long r = load(fname);
        if (r >= 0) printf("ted: %s: %d lines, %ld bytes\n", fname, nlines, r);
        else printf("ted: %s: new file\n", fname);
    }
    cur = nlines;
    char cmd[MAX_LINE];
    bool warned = false;
    while (getline0(cmd, sizeof cmd) || !ieof) {
        const char *p = cmd;
        int a, b, given = 0;
        if (*p == ',' || *p == '%') {
            a = 1; b = nlines; p++; given = 2;
            if (nlines == 0) a = 0;
        } else {
            a = parse_addr(&p);
            if (a == -1) { a = b = cur; }
            else {
                given = 1; b = a;
                if (*p == ',') {
                    p++;
                    b = parse_addr(&p);
                    if (b == -1) { err("bad range"); continue; }
                    given = 2;
                }
            }
        }
        char c = *p ? *p++ : 0;
        while (*p == ' ') p++;
        bool need_lines = c == 'd' || c == 'p' || c == 'n' || c == 'c' || (c == 0 && given);
        if (need_lines && (a < 1 || b > nlines || a > b)) { err("invalid address"); continue; }
        if ((c == 'a' || c == 'i') && (a < 0 || a > nlines)) { err("invalid address"); continue; }
        if (c != 'q') warned = false;
        switch (c) {
        case 0:
            if (!given) {                     /* empty line: next line, like ed */
                if (cur >= nlines) { err("end of buffer"); break; }
                a = b = cur + 1;
            }
            print_range(b, b, false);
            break;
        case 'p': print_range(a, b, false); break;
        case 'n': print_range(a, b, true); break;
        case '=': printf("%d\n", nlines); break;
        case 'a': input_mode(a); break;
        case 'i': input_mode(a > 0 ? a - 1 : 0); break;
        case 'c':
            delete_lines(a, b); dirty = true;
            input_mode(a - 1);
            break;
        case 'd':
            delete_lines(a, b); dirty = true;
            cur = a <= nlines ? a : nlines;
            break;
        case 'w': {
            if (*p) snprintf(fname, sizeof fname, "%s", p);
            if (!fname[0]) { err("no file name"); break; }
            long r = save(fname);
            if (r < 0) { printf("? %s: write error %ld\n", fname, r); break; }
            printf("%ld\n", r);
            dirty = false;
            break;
        }
        case 'q':
            if (dirty && !warned) { err("unsaved changes (q again or Q to quit)"); warned = true; break; }
            return 0;
        case 'Q': return 0;
        default: err("unknown command");
        }
    }
    return dirty ? 1 : 0;
}
