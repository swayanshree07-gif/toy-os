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
 * stage's. Day 13 session: the output-only builtins (pwd, status, help, jobs) may
 * be pipeline stages: the program stages are spawned first, then the shell itself
 * runs the builtin with its fds 0-2 swapped for the stage's pipe ends (dup2), so it
 * sees the real shell state (e.g. `jobs | wc`). Builtins that change the shell
 * (cd, exit, exec, fg, wait) are rejected in pipelines.
 * Background jobs (Day 12 session): a command line ending in `&` (a program or a
 * pipeline) starts without waiting; it stays in its own process group, so Ctrl-C does
 * not reach it, and its stdin is /dev/null unless redirected. The shell prints
 * "[N] PGID", and reports "[N] done ..." (or exit status / interrupted / killed) when
 * the job has finished, checked with waitpid(WNOHANG) before each prompt or script
 * line. Builtins: `jobs` lists them, `fg [%N]` waits for one in the foreground (Ctrl-C
 * stops it), `wait` waits for all of them the same way.
 * `exec prog args` replaces the shell with prog (same pid, fds and cwd). */
#include "tern.h"

#define LINE_MAX 256
#define ARGS_MAX 16
#define STAGES_MAX 8
#define JOBS_MAX 8
#define JOB_CMD_MAX 48

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
    case EROFS: return "read-only file system";
    default: return "error";
    }
}

static void help(void) {
    printf("tsh builtins: cd [dir]  pwd  exit [code]  status  help  exec prog [args]\n"
           "redirection: prog < in > out (>> appends, 2> stderr); Ctrl-C stops prog\n"
           "pipelines: prog1 | prog2 | ...  (e.g. ls /bin | grep t | wc; pwd/status/help/jobs too)\n"
           "jobs: prog &  (background)  jobs  fg [%%N]  wait\n"
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
    int pid;                   /* 0 = builtin run by the shell, -1 = not started */
    int bfds[3];               /* builtin stage: its fds 0-2 (-1 = the shell's own) */
};

static void program_path(const char *word, char *path, size_t max) {
    if (strchr(word, '/')) snprintf(path, max, "%s", word);
    else snprintf(path, max, "/bin/%s", word);
}

/* ---- background jobs ---- */
struct job {
    int id;                    /* 0 = free slot */
    int pgid;
    int pids[STAGES_MAX];      /* -1 once reaped */
    int n;
    int status;                /* the last stage's */
    char cmd[JOB_CMD_MAX];
};
static struct job jobs[JOBS_MAX];

static struct job *job_new(void) {
    struct job *free_slot = NULL;
    for (int id = 1; id <= JOBS_MAX; id++) {       /* smallest unused id */
        bool used = false;
        for (int i = 0; i < JOBS_MAX; i++) if (jobs[i].id == id) used = true;
        if (used) continue;
        for (int i = 0; i < JOBS_MAX; i++)
            if (!jobs[i].id) { free_slot = &jobs[i]; break; }
        if (free_slot) { memset(free_slot, 0, sizeof *free_slot); free_slot->id = id; }
        break;
    }
    return free_slot;
}

static bool job_running(const struct job *j) {
    for (int i = 0; i < j->n; i++) if (j->pids[i] > 0) return true;
    return false;
}

/* Collect exited stages of job j (blocking or not); once all are gone, report the job
 * and free its slot. Returns true if the job is finished. */
static bool job_collect(struct job *j, bool block) {
    for (int i = 0; i < j->n; i++) {
        if (j->pids[i] <= 0) continue;
        int st = 0;
        int rc = waitpid(j->pids[i], &st, block ? 0 : TERN_WNOHANG);
        if (rc == 0) continue;                      /* still running */
        if (rc == j->pids[i] && i == j->n - 1) j->status = st;
        j->pids[i] = -1;
    }
    if (job_running(j)) return false;
    if (j->status == 0) printf("[%d] done  %s\n", j->id, j->cmd);
    else if (j->status == TERN_EXIT_INTR) printf("[%d] interrupted  %s\n", j->id, j->cmd);
    else if (j->status == TERN_EXIT_KILLED) printf("[%d] killed  %s\n", j->id, j->cmd);
    else printf("[%d] exit %d  %s\n", j->id, j->status, j->cmd);
    j->id = 0;
    return true;
}

static void reap_jobs(void) {
    for (int i = 0; i < JOBS_MAX; i++)
        if (jobs[i].id) job_collect(&jobs[i], false);
}

/* Wait for job j with it as the foreground group (Ctrl-C stops it). */
static void job_wait_fg(struct job *j) {
    setfg(j->pgid);
    job_collect(j, true);
    last_status = j->status;
    setfg(0);
}

static bool is_builtin(const char *w);
static bool run_builtin(int argc, char **argv, int *code);
static void builtin_restore(void);
static int saved_std[3] = { -1, -1, -1 };

/* Run n program stages connected by pipes and wait for all of them, or (bg) record
 * them as a background job. */
static void run_pipeline(struct stage *sg, int n, bool bg, const char *cmd) {
    struct job *job = NULL;
    if (bg && !(job = job_new())) { printf("tsh: too many jobs\n"); last_status = 1; return; }
    int prev_rd = -1;             /* read end of the pipe from the previous stage */
    int pgid = 0, spawned = 0;
    bool broken = false;
    for (int i = 0; i < n; i++) {
        sg[i].pid = -1;
        sg[i].bfds[0] = sg[i].bfds[1] = sg[i].bfds[2] = -1;
        char path[LINE_MAX];
        char **argv = sg[i].argv;
        program_path(argv[0], path, sizeof path);
        int fds[3], pp[2] = { -1, -1 };
        if (!open_redirs(&sg[i].rd, fds)) { last_status = 1; broken = true; break; }
        if (i + 1 < n) {
            int rc = pipe(pp);
            if (rc < 0) { printf("tsh: pipe: %s\n", errstr(rc)); close_fds(fds); last_status = 1; broken = true; break; }
        }
        if (fds[0] < 0 && prev_rd >= 0) { fds[0] = prev_rd; prev_rd = -1; }
        if (fds[0] < 0 && bg) fds[0] = open("/dev/null", O_RDONLY);  /* no console input */
        if (fds[1] < 0 && pp[1] >= 0) { fds[1] = pp[1]; pp[1] = -1; }
        if (is_builtin(argv[0])) {     /* run later by the shell itself */
            if (fds[0] >= 0) { close(fds[0]); fds[0] = -1; }  /* builtins don't read */
            for (int k = 0; k < 3; k++) sg[i].bfds[k] = fds[k];
            if (prev_rd >= 0) close(prev_rd);
            prev_rd = pp[0];
            sg[i].pid = 0;
            continue;
        }
        int pid = spawn_fds(path, (const char *const *)argv, fds);
        close_fds(fds);                /* the child holds its own references */
        if (prev_rd >= 0) close(prev_rd);
        if (pp[1] >= 0) close(pp[1]);
        prev_rd = pp[0];
        if (pid < 0) {
            printf("tsh: %s: %s\n", argv[0], errstr(pid));
            last_status = 127;
            broken = true;
            break;
        }
        if (!pgid) pgid = pid;
        setpgid(pid, pgid);
        if (spawned == 0 && !bg) setfg(pgid);
        sg[i].pid = pid;
        spawned++;
    }
    if (prev_rd >= 0) close(prev_rd);  /* (only left over when a stage failed) */
    int bstatus = -1;
    for (int i = 0; i < n; i++) {      /* builtin stages, now that their readers run */
        if (sg[i].pid != 0) continue;
        bool ok = !broken;           /* skip if the pipeline broke */
        if (ok) {
            for (int k = 0; k < 3; k++) {
                if (sg[i].bfds[k] < 0) continue;
                saved_std[k] = dup(k);
                dup2(sg[i].bfds[k], k);
            }
            int argc = 0, dummy = 0;
            while (sg[i].argv[argc]) argc++;
            run_builtin(argc, sg[i].argv, &dummy);
            builtin_restore();
            if (i == n - 1) bstatus = last_status;
        }
        close_fds(sg[i].bfds);
        sg[i].pid = -1;
    }
    if (bg) {
        if (!spawned) { job->id = 0; return; }
        job->pgid = pgid;
        job->n = 0;
        for (int i = 0; i < n; i++) if (sg[i].pid > 0) job->pids[job->n++] = sg[i].pid;
        snprintf(job->cmd, sizeof job->cmd, "%s", cmd);
        printf("[%d] %d\n", job->id, pgid);
        last_status = 0;
        return;
    }
    for (int i = 0; i < n; i++) {
        if (sg[i].pid < 0) continue;
        int st = 0;
        if (wait(sg[i].pid, &st) != sg[i].pid) { printf("tsh: wait failed\n"); continue; }
        if (i == n - 1) last_status = st;
        report(sg[i].argv[0], st);
    }
    if (bstatus >= 0) last_status = bstatus;
    setfg(0);
}

static bool is_builtin(const char *w) {
    return !strcmp(w, "exit") || !strcmp(w, "cd") || !strcmp(w, "pwd") ||
           !strcmp(w, "status") || !strcmp(w, "help") || !strcmp(w, "jobs") ||
           !strcmp(w, "fg") || !strcmp(w, "wait") || !strcmp(w, "exec");
}

/* Builtins that only print, allowed as pipeline stages. */
static bool pipe_builtin(const char *w) {
    return !strcmp(w, "pwd") || !strcmp(w, "status") || !strcmp(w, "help") || !strcmp(w, "jobs");
}

/* Remove a trailing '&' (outside quotes, before any comment). True if there was one. */
static bool strip_bg(char *line) {
    bool q = false;
    char *end = line;
    while (*end) {
        if (*end == '"') q = !q;
        else if (!q && *end == '#') break;
        end++;
    }
    while (end > line && (end[-1] == ' ' || end[-1] == '\t')) end--;
    if (end > line && end[-1] == '&') { end[-1] = 0; return true; }
    return false;
}

/* The command text of a line for the job table: blanks squeezed, comment dropped. */
static void job_text(const char *line, char *out, size_t max) {
    size_t n = 0;
    bool q = false, blank = false;
    for (const char *p = line; *p && n + 1 < max; p++) {
        if (*p == '"') q = !q;
        if (!q && *p == '#') break;
        if (*p == ' ' || *p == '\t') { blank = n > 0; continue; }
        if (blank && n + 2 < max) out[n++] = ' ';
        blank = false;
        out[n++] = *p;
    }
    out[n] = 0;
}

static void builtin_jobs(void) {
    reap_jobs();
    for (int id = 1; id <= JOBS_MAX; id++)
        for (int i = 0; i < JOBS_MAX; i++)
            if (jobs[i].id == id) printf("[%d] running  %d  %s\n", id, jobs[i].pgid, jobs[i].cmd);
    last_status = 0;
}

/* fg [%N]: the given job, or the newest one. */
static void builtin_fg(int argc, char **argv) {
    struct job *j = NULL;
    if (argc > 1) {
        int id = atoi(argv[1][0] == '%' ? argv[1] + 1 : argv[1]);
        for (int i = 0; i < JOBS_MAX; i++) if (jobs[i].id && jobs[i].id == id) j = &jobs[i];
    } else {
        for (int i = 0; i < JOBS_MAX; i++) if (jobs[i].id && (!j || jobs[i].id > j->id)) j = &jobs[i];
    }
    if (!j) { printf("tsh: fg: no such job\n"); last_status = 1; return; }
    printf("%s\n", j->cmd);
    job_wait_fg(j);
}

static void builtin_wait(void) {
    int st = 0;
    for (int id = 1; id <= JOBS_MAX; id++)
        for (int i = 0; i < JOBS_MAX; i++)
            if (jobs[i].id == id) { job_wait_fg(&jobs[i]); st = last_status; }
    last_status = st;
}

static void builtin_exec(int argc, char **argv, const struct redir *rd) {
    if (argc < 2) { last_status = 0; return; }
    if (rd->in || rd->out || rd->err) { printf("tsh: exec: redirection not supported\n"); last_status = 2; return; }
    char path[LINE_MAX];
    program_path(argv[1], path, sizeof path);
    int rc = exec(path, (const char *const *)argv + 1);
    printf("tsh: exec: %s: %s\n", argv[1], errstr(rc));
    last_status = 127;
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

/* Builtins with redirections (Day 12 session): the shell's own fds 0-2 are saved with
 * dup, the redirection files put in their place with dup2, and restored afterwards. */
static bool builtin_redirect(const struct redir *rd) {
    if (!rd->in && !rd->out && !rd->err) return true;
    int fds[3];
    if (!open_redirs(rd, fds)) return false;
    for (int i = 0; i < 3; i++) {
        if (fds[i] < 0) continue;
        saved_std[i] = dup(i);
        dup2(fds[i], i);
        close(fds[i]);
    }
    return true;
}

static void builtin_restore(void) {
    for (int i = 0; i < 3; i++) {
        if (saved_std[i] < 0) continue;
        dup2(saved_std[i], i);
        close(saved_std[i]);
        saved_std[i] = -1;
    }
}

/* Runs builtin argv[0] (not exec). Returns false when the shell should exit. */
static bool run_builtin(int argc, char **argv, int *code) {
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
    } else if (!strcmp(argv[0], "jobs")) {
        builtin_jobs();
    } else if (!strcmp(argv[0], "fg")) {
        builtin_fg(argc, argv);
    } else if (!strcmp(argv[0], "wait")) {
        builtin_wait();
    }
    return true;
}

/* Returns false when the shell should exit (exit code in *code). */
static bool run_line(char *line, int *code) {
    static struct stage sg[STAGES_MAX];
    char *seg[STAGES_MAX];
    char cmd[JOB_CMD_MAX];
    bool bg = strip_bg(line);
    job_text(line, cmd, sizeof cmd);
    int nseg = split_pipes(line, seg, STAGES_MAX);
    if (nseg < 0) { printf("tsh: too many pipeline stages\n"); last_status = 2; return true; }
    int argc = 0;
    for (int i = 0; i < nseg; i++) {
        memset(&sg[i].rd, 0, sizeof sg[i].rd);
        argc = parse(seg[i], sg[i].argv, &sg[i].rd);
        if (argc < 0) { last_status = 2; return true; }
        if (argc == 0 && nseg > 1) { printf("tsh: empty pipeline stage\n"); last_status = 2; return true; }
        if (nseg > 1 && is_builtin(sg[i].argv[0]) && !pipe_builtin(sg[i].argv[0])) {
            printf("tsh: %s: builtins can't be used in a pipeline\n", sg[i].argv[0]);
            last_status = 2;
            return true;
        }
        if (bg && argc > 0 && is_builtin(sg[i].argv[0])) {
            printf("tsh: %s: builtins can't run in the background\n", sg[i].argv[0]);
            last_status = 2;
            return true;
        }
    }
    if (argc == 0) {
        if (bg) { printf("tsh: & without a command\n"); last_status = 2; }
        return true;
    }
    if (bg && is_builtin(sg[0].argv[0])) {
        printf("tsh: %s: builtins can't run in the background\n", sg[0].argv[0]);
        last_status = 2;
        return true;
    }
    if (nseg > 1 || bg || !is_builtin(sg[0].argv[0])) { run_pipeline(sg, nseg, bg, cmd); return true; }
    char **argv = sg[0].argv;
    if (!strcmp(argv[0], "exec")) { builtin_exec(argc, argv, &sg[0].rd); return true; }
    if (!builtin_redirect(&sg[0].rd)) { last_status = 1; return true; }
    bool go_on = run_builtin(argc, argv, code);
    builtin_restore();
    return go_on;
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
            reap_jobs();
            char cwd[LINE_MAX];
            if (getcwd(cwd, sizeof cwd) < 0) snprintf(cwd, sizeof cwd, "?");
            printf("tern:%s$ ", cwd);
        }
        if (read_line(line, sizeof line) < 0) { code = last_status; break; }
        reap_jobs();                    /* report background jobs that have finished */
        if (script) printf("+ %s\n", line);
        if (!run_line(line, &code)) break;
    }
    if (!script) printf("tsh: bye\n");
    return code;
}
