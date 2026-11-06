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
 * Ctrl-C kills it (status 130). */
#include "tern.h"

#define LINE_MAX 256
#define ARGS_MAX 16

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
    default: return "error";
    }
}

static void help(void) {
    printf("tsh builtins: cd [dir]  pwd  exit [code]  status  help\n"
           "redirection: prog < in > out (>> appends, 2> stderr); Ctrl-C stops prog\n"
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

static void run_program(char **argv, const struct redir *rd) {
    char path[LINE_MAX];
    if (strchr(argv[0], '/')) snprintf(path, sizeof path, "%s", argv[0]);
    else snprintf(path, sizeof path, "/bin/%s", argv[0]);
    int fds[3];
    if (!open_redirs(rd, fds)) { last_status = 1; return; }
    int pid = spawn_fds(path, (const char *const *)argv, fds);
    close_fds(fds);                    /* the child holds its own references */
    if (pid < 0) {
        printf("tsh: %s: %s\n", argv[0], errstr(pid));
        last_status = 127;
        return;
    }
    setfg(pid);
    int st = 0;
    int r = wait(pid, &st);
    setfg(0);
    if (r != pid) { printf("tsh: wait failed\n"); return; }
    last_status = st;
    if (st == TERN_EXIT_INTR) printf("tsh: %s: interrupted\n", argv[0]);
    else if (st == TERN_EXIT_KILLED) printf("tsh: %s: killed\n", argv[0]);
    else if (st >= 128 && st < 160) printf("tsh: %s: killed (exception %d)\n", argv[0], st - 128);
}

/* Returns false when the shell should exit (exit code in *code). */
static bool run_line(char *line, int *code) {
    char *argv[ARGS_MAX + 1];
    struct redir rd = { 0 };
    int argc = parse(line, argv, &rd);
    if (argc <= 0) { if (argc < 0) last_status = 2; return true; }
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
        run_program(argv, &rd);
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
