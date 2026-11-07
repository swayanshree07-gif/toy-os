/* /bin/tsh: the TernOS shell (plan Day 23).
 *
 *   tsh            interactive: prompt, reads lines from the console
 *   tsh FILE       runs the commands in FILE (each echoed as "+ cmd"), then exits
 *
 * A line is split into words at blanks; "double quotes" group words, '#' starts a
 * comment. Builtins: cd [dir], pwd, exit [code], help. Anything else is a program:
 * a word containing '/' is a path, otherwise /bin/<word>. The shell waits for it and
 * remembers its exit status ("status" builtin prints it).
 * Redirections for programs: `< file` (stdin), `> file` (stdout, truncate),
 * `>> file` (stdout, append), `2> file` (stderr). The program runs in the foreground:
 * Ctrl-C kills it (status 130).
 * Pipelines (Day 10 session): `a | b | c` connects stdout of each program to stdin of
 * the next through kernel pipes (a redirection given for a stage wins over the pipe).
 * All stages share one process group (the first stage's pid), which becomes the
 * foreground group, so Ctrl-C stops the whole pipeline. The status is the last
 * stage's. Builtins can't be used in a pipeline. */
#include "tern.h"

#define LINE_MAX 256
#define ARGS_MAX 16
#define STAGES_MAX 8

static int last_status;

/* ---- buffered line reader over an fd ---- */
static char ibuf[512];
static long ilen, ipos;
static int in_fd;

/* Read one line without its '\n' into line. Returns length, or -1 at end of input. */
static long read_line(char *line, size_t max) {
    size_t n = 0;
    for (;;) {
        if (ipos == ilen) {
            ilen = read(in_fd, ibuf, sizeof ibuf);
            ipos = 0;
            if (ilen <= 0) { ilen = 0; return n ? (long)n : -1; }
        }
        char c = ibuf[ipos++];
        if (c == '\n') break;
        if (n + 1 < max) line[n++] = c;
    }
    line[n] = 0;
    return (long)n;
}

struct redir {
    char *in, *out, *err;     /* file names, NULL = console */
    bool append;
};

/* Split line in place. Returns the number of words, or -1 on a syntax error. */
static int parse(char *line, char **argv, struct redir *rd) {
    int argc = 0;
    char *p = line;
    char **target = NULL;     /* redirection waiting for its file name */
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') break;
        if (!target && (*p == '<' || *p == '>' || (p[0] == '2' && p[1] == '>'))) {
            if (*p == '<') { target = &rd->in; p++; }
            else if (*p == '2') { target = &rd->err; p += 2; }
            else if (p[1] == '>') { target = &rd->out; rd->append = true; p += 2; }
            else { target = &rd->out; rd->append = false; p++; }
            continue;          /* the file name may follow directly or after blanks */
        }
        if (!target && argc == ARGS_MAX) { printf("tsh: too many arguments\n"); return -1; }
        char *out = p;
        if (target) { *target = out; target = NULL; }
        else argv[argc++] = out;
        while (*p && *p != ' ' && *p != '\t') {
            if (*p == '"') {
                p++;
                while (*p && *p != '"') *out++ = *p++;
                if (*p != '"') { printf("tsh: missing closing quote\n"); return -1; }
                p++;
            } else {
                *out++ = *p++;
            }
        }
        if (*p) p++;
        *out = 0;
    }
    argv[argc] = NULL;
    if (target) { printf("tsh: missing file name after redirection\n"); return -1; }
    if (argc == 0 && (rd->in || rd->out || rd->err)) { printf("tsh: redirection without a command\n"); return -1; }
    return argc;
}

static const char *errstr(int e) {
    switch (-e) {
    case ENOENT: return "not found";
    case EACCES: return "permission denied";
    case ENOEXEC: return "not an executable";
    case ENOTDIR: return "not a directory";
    case ENOMEM: return "out of memory";
    case EINVAL: return "invalid argument";
    case EISDIR: return "is a directory";
    case EEXIST: return "already exists";
    case EAGAIN: return "too many processes";
    case EMFILE: return "too many open files";
    default: return "error";
    }
}

static void help(void) {
    printf("tsh builtins: cd [dir]  pwd  exit [code]  status  help\n"
           "redirection: prog < in > out (>> appends, 2> stderr); Ctrl-C stops prog\n"
           "pipelines: prog1 | prog2 | ...  (e.g. ls /bin | grep t | wc)\n"
           "programs in /bin: ");
    int fd = open("/bin", O_RDONLY | O_DIRECTORY);
    struct tern_dirent d;
    while (fd >= 0 && readdir(fd, &d) == 1) printf("%s ", d.name);
    if (fd >= 0) close(fd);
    printf("\n");
}

static void close_fds(int *fds) {
    for (int i = 0; i < 3; i++)
        if (fds[i] >= 0) close(fds[i]);
}

/* Open the redirection files into fds[0..2] (-1 = console). false on error. */
static bool open_redirs(const struct redir *rd, int *fds) {
    fds[0] = fds[1] = fds[2] = -1;
    const char *names[3] = { rd->in, rd->out, rd->err };
    for (int i = 0; i < 3; i++) {
        if (!names[i]) continue;
        int flags = i == 0 ? O_RDONLY
                  : O_WRONLY | O_CREAT | (i == 1 && rd->append ? O_APPEND : O_TRUNC);
        fds[i] = open(names[i], flags);
        if (fds[i] < 0) {
            printf("tsh: %s: %s\n", names[i], errstr(fds[i]));
            fds[i] = -1;
            close_fds(fds);
            return false;
        }
    }
    return true;
}

static void report(const char *name, int st) {
    if (st == TERN_EXIT_INTR) printf("tsh: %s: interrupted\n", name);
    else if (st == TERN_EXIT_KILLED) printf("tsh: %s: killed\n", name);
    else if (st >= 128 && st < 160) printf("tsh: %s: killed (exception %d)\n", name, st - 128);
}

struct stage {
    char *argv[ARGS_MAX + 1];
    struct redir rd;
    int pid;
};

/* Run n program stages connected by pipes, wait for all of them. */
static void run_pipeline(struct stage *sg, int n) {
    int prev_rd = -1;             /* read end of the pipe from the previous stage */
    int pgid = 0, spawned = 0;
    for (int i = 0; i < n; i++) {
        sg[i].pid = -1;
        char path[LINE_MAX];
        char **argv = sg[i].argv;
        if (strchr(argv[0], '/')) snprintf(path, sizeof path, "%s", argv[0]);
        else snprintf(path, sizeof path, "/bin/%s", argv[0]);
        int fds[3], pp[2] = { -1, -1 };
        if (!open_redirs(&sg[i].rd, fds)) { last_status = 1; break; }
        if (i + 1 < n) {
            int rc = pipe(pp);
            if (rc < 0) { printf("tsh: pipe: %s\n", errstr(rc)); close_fds(fds); last_status = 1; break; }
        }
        if (fds[0] < 0 && prev_rd >= 0) { fds[0] = prev_rd; prev_rd = -1; }
        if (fds[1] < 0 && pp[1] >= 0) { fds[1] = pp[1]; pp[1] = -1; }
        int pid = spawn_fds(path, (const char *const *)argv, fds);
        close_fds(fds);                /* the child holds its own references */
        if (prev_rd >= 0) close(prev_rd);
        if (pp[1] >= 0) close(pp[1]);
        prev_rd = pp[0];
        if (pid < 0) {
            printf("tsh: %s: %s\n", argv[0], errstr(pid));
            last_status = 127;
            break;
        }
        if (!pgid) pgid = pid;
        setpgid(pid, pgid);
        if (spawned == 0) setfg(pgid);
        sg[i].pid = pid;
        spawned++;
    }
    if (prev_rd >= 0) close(prev_rd);  /* (only left over when a stage failed) */
    for (int i = 0; i < n; i++) {
        if (sg[i].pid < 0) continue;
        int st = 0;
        if (wait(sg[i].pid, &st) != sg[i].pid) { printf("tsh: wait failed\n"); continue; }
        if (i == n - 1) last_status = st;
        report(sg[i].argv[0], st);
    }
    setfg(0);
}

static bool is_builtin(const char *w) {
    return !strcmp(w, "exit") || !strcmp(w, "cd") || !strcmp(w, "pwd") ||
           !strcmp(w, "status") || !strcmp(w, "help");
}

/* Split at '|' outside quotes and comments. Returns the number of segments. */
static int split_pipes(char *line, char **seg, int max) {
    int n = 0;
    seg[n++] = line;
    bool q = false;
    for (char *p = line; *p; p++) {
        if (*p == '"') q = !q;
        else if (!q && *p == '#') break;
        else if (!q && *p == '|') {
            if (n == max) return -1;
            *p = 0;
            seg[n++] = p + 1;
        }
    }
    return n;
}

/* Returns false when the shell should exit (exit code in *code). */
static bool run_line(char *line, int *code) {
    static struct stage sg[STAGES_MAX];
    char *seg[STAGES_MAX];
    int nseg = split_pipes(line, seg, STAGES_MAX);
    if (nseg < 0) { printf("tsh: too many pipeline stages\n"); last_status = 2; return true; }
    int argc = 0;
    for (int i = 0; i < nseg; i++) {
        memset(&sg[i].rd, 0, sizeof sg[i].rd);
        argc = parse(seg[i], sg[i].argv, &sg[i].rd);
        if (argc < 0) { last_status = 2; return true; }
        if (argc == 0 && nseg > 1) { printf("tsh: empty pipeline stage\n"); last_status = 2; return true; }
        if (nseg > 1 && is_builtin(sg[i].argv[0])) {
            printf("tsh: %s: builtins can't be used in a pipeline\n", sg[i].argv[0]);
            last_status = 2;
            return true;
        }
    }
    if (argc == 0) return true;
    if (nseg > 1) { run_pipeline(sg, nseg); return true; }
    char **argv = sg[0].argv;
    if (!strcmp(argv[0], "exit")) {
        *code = argc > 1 ? atoi(argv[1]) : last_status;
        return false;
    } else if (!strcmp(argv[0], "cd")) {
        int rc = chdir(argc > 1 ? argv[1] : "/");
        if (rc) printf("tsh: cd: %s: %s\n", argc > 1 ? argv[1] : "/", errstr(rc));
        last_status = rc ? 1 : 0;
    } else if (!strcmp(argv[0], "pwd")) {
        char cwd[LINE_MAX];
        if (getcwd(cwd, sizeof cwd) >= 0) printf("%s\n", cwd);
        last_status = 0;
    } else if (!strcmp(argv[0], "status")) {
        printf("%d\n", last_status);
    } else if (!strcmp(argv[0], "help")) {
        help();
        last_status = 0;
    } else {
        run_pipeline(sg, 1);
    }
    return true;
}

int main(int argc, char **argv) {
    bool script = argc > 1;
    in_fd = 0;
    if (script) {
        in_fd = open(argv[1], O_RDONLY);
        if (in_fd < 0) { printf("tsh: %s: %s\n", argv[1], errstr(in_fd)); return 127; }
    } else {
        printf("tsh: TernOS shell. Type 'help' for commands.\n");
    }
    char line[LINE_MAX];
    int code = 0;
    for (;;) {
        if (!script) {
            char cwd[LINE_MAX];
            if (getcwd(cwd, sizeof cwd) < 0) snprintf(cwd, sizeof cwd, "?");
            printf("tern:%s$ ", cwd);
        }
        if (read_line(line, sizeof line) < 0) { code = last_status; break; }
        if (script) printf("+ %s\n", line);
        if (!run_line(line, &code)) break;
    }
    if (!script) printf("tsh: bye\n");
    return code;
}
