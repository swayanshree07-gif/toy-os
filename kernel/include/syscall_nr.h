#ifndef TERN_SYSCALL_NR_H
#define TERN_SYSCALL_NR_H
/* System call numbers and error codes, shared by the kernel and user programs
 * (C and assembly). ABI (like Linux x86-64): number in rax, arguments in rdi, rsi,
 * rdx, r10, r8, r9; `syscall`; result in rax (negative = -errno). rcx and r11 are
 * clobbered, every other register is preserved. */
#define SYS_exit      0     /* exit(code)                     never returns */
#define SYS_write     1     /* write(fd, buf, len) -> bytes   fd 1/2 = console */
#define SYS_getpid    2     /* getpid() -> pid */
#define SYS_yield     3     /* yield() -> 0 */
#define SYS_sleep_ms  4     /* sleep_ms(ms) -> 0 */
#define SYS_uptime_ms 5     /* uptime_ms() -> ms since boot */
/* Day 18: files. Paths are NUL-terminated, relative to the process's cwd. */
#define SYS_open      6     /* open(path, flags) -> fd */
#define SYS_close     7     /* close(fd) -> 0 */
#define SYS_read      8     /* read(fd, buf, len) -> bytes (0 = end of file) */
#define SYS_lseek     9     /* lseek(fd, off, whence) -> new offset */
#define SYS_readdir   10    /* readdir(fd, struct tern_dirent *) -> 1 entry, 0 at end */
#define SYS_mkdir     11    /* mkdir(path) -> 0 */
#define SYS_unlink    12    /* unlink(path) -> 0 (files or empty directories) */
#define SYS_stat      13    /* stat(path, struct tern_stat *) -> 0 */
#define SYS_fstat     14    /* fstat(fd, struct tern_stat *) -> 0 */
#define SYS_brk       15    /* brk(addr) -> new break; brk(0) queries. Heap grows up from
                               the end of the program image, max USER_HEAP_MAX bytes */
/* Day 21: processes */
#define SYS_spawn     16    /* spawn(path, argv, fds) -> child pid. argv: NULL-terminated
                               array (max 32 entries, 2 KiB of strings); NULL = no
                               arguments. fds: NULL, or int[3] giving the child's fds 0-2:
                               a caller fd to share, or -1 = fresh /dev/console. NULL =
                               all three on the console. The child gets the caller's cwd */
#define SYS_wait      17    /* wait(pid or -1, int *status, flags) -> pid of the reaped
                               child; -ECHILD if there is no such child. status may be
                               NULL. flags TERN_WNOHANG: return 0 instead of blocking */
#define SYS_getppid   18    /* getppid() -> parent pid, 0 if none (orphan / kernel) */
#define SYS_chdir     19    /* chdir(path) -> 0 */
#define SYS_getcwd    20    /* getcwd(buf, size) -> length (without NUL) */
/* Day 9 session (plan Days 24-26): signals-lite, system information */
#define SYS_kill      21    /* kill(pid) -> 0; the process exits with TERN_EXIT_KILLED the
                               next time it would return to ring 3 (blocking tty reads,
                               wait and sleep are interrupted). -ESRCH if no such process */
#define SYS_setfg     22    /* setfg(pid) -> previous foreground pid. Ctrl-C on the console
                               kills the foreground process (TERN_EXIT_INTR); 0 = none, then
                               Ctrl-C just discards the line being edited */
#define SYS_sysinfo   23    /* sysinfo(struct tern_sysinfo *) -> 0 */
#define SYS_proclist  24    /* proclist(struct tern_procinfo *buf, max) -> number of
                               processes (fills at most max entries) */
/* Day 10 session: pipes, process groups, rename */
#define SYS_pipe      25    /* pipe(int fds[2]) -> 0; fds[0] = read end, fds[1] = write end */
#define SYS_setpgid   26    /* setpgid(pid, pgid) -> 0. pid 0 = self; pid must be the caller
                               or one of its children; pgid 0 = pid. Every process starts
                               in its own group (pgid = pid). setfg() names a group: Ctrl-C
                               kills every process in it */
#define SYS_getpgid   27    /* getpgid(pid) -> pgid (pid 0 = self) */
#define SYS_rename    28    /* rename(from, to) -> 0 (POSIX semantics, see vfs.h) */
/* Day 11 session: framebuffer access for user programs, and power off */
#define SYS_fbinfo    29    /* fbinfo(struct tern_fbinfo *) -> 0, -ENODEV without a framebuffer */
#define SYS_fbacquire 30    /* fbacquire(1) takes the framebuffer (the text console stops
                               drawing; -EBUSY if another process has it); fbacquire(0)
                               gives it back and the console redraws. Released at exit. */
#define SYS_fbblit    31    /* fbblit(const struct tern_fbblit *) -> 0; owner only (-EPERM).
                               Pixels are 0x00RRGGBB; the kernel converts if needed. */
#define SYS_poweroff  32    /* poweroff() -> ACPI S5 (QEMU exits); -EIO if that failed */
/* Day 12 session */
#define SYS_exec      33    /* exec(path, argv) replaces the program of the calling process
                               (same pid, parent, pgid, fds, cwd); argv as for spawn. Does
                               not return on success; on failure nothing has changed */
#define SYS_dup       34    /* dup(fd) -> lowest free fd, sharing fd's open file (offset) */
#define SYS_dup2      35    /* dup2(fd, newfd) -> newfd; newfd is closed first if open;
                               dup2(fd, fd) just checks fd */
#define SYS_COUNT     36
#define TERN_WNOHANG  1
#define PROC_MAX_LIVE 64    /* spawn fails with -EAGAIN beyond this many processes */
#define TERN_EXIT_INTR   130   /* killed by Ctrl-C */
#define TERN_EXIT_KILLED 137   /* killed by kill() */
#define USER_HEAP_MAX (64ull << 20)

#define ENOENT  2
#define ESRCH   3
#define EINTR   4
#define EIO     5
#define EBADF   9
#define ECHILD  10
#define EAGAIN  11
#define EBUSY   16
#define EPIPE   32
#define ERANGE  34
#define ENOMEM  12
#define EACCES  13
#define EFAULT  14
#define EEXIST  17
#define ENOTDIR 20
#define EISDIR  21
#define EPERM   1
#define ENODEV  19
#define EINVAL  22
#define EMFILE  24
#define ENOSPC  28
#define ENAMETOOLONG 36
#define ENOSYS  38
#define ENOTEMPTY 39
#define ENOEXEC 8

/* open flags (Linux values) */
#define O_RDONLY   0
#define O_WRONLY   1
#define O_RDWR     2
#define O_ACCMODE  3
#define O_CREAT    0x40
#define O_TRUNC    0x200
#define O_APPEND   0x400
#define O_DIRECTORY 0x10000

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define TERN_FT_FILE 1
#define TERN_FT_DIR  2
#define TERN_FT_DEV  3

#ifndef __ASSEMBLER__
#include <stdint.h>
struct tern_stat {
    uint32_t type;           /* TERN_FT_* */
    uint32_t nlink;          /* directories: number of entries */
    uint64_t size;
    uint64_t ino;
};
struct tern_dirent {
    uint32_t type;           /* TERN_FT_* */
    uint32_t reserved;
    uint64_t size;
    char name[56];           /* NUL-terminated */
};
struct tern_sysinfo {
    uint64_t uptime_ms;
    uint64_t frames_total, frames_free;      /* 4 KiB physical frames */
    uint64_t heap_mapped, heap_used, heap_free;  /* kernel heap, bytes */
    uint64_t procs, threads;                 /* live processes / kernel threads */
    uint64_t switches, preemptions;          /* scheduler counters since boot */
    uint64_t vfs_nodes, vfs_bytes;           /* RAM FS nodes and file data bytes */
    uint64_t pipes_live, pipes_created, pipe_bytes;  /* Day 10 session */
};
struct tern_fbinfo {
    uint32_t width, height;      /* pixels */
    uint32_t owner;              /* pid holding the framebuffer, 0 = text console */
    uint32_t reserved;
};
struct tern_fbblit {
    const uint32_t *pixels;      /* w x h pixels, row i at (char *)pixels + i * stride */
    uint32_t x, y, w, h;         /* destination rectangle, must lie inside the screen */
    uint32_t stride;             /* bytes, >= 4 * w */
    uint32_t reserved;
};
#define TERN_PS_RUNNING  0
#define TERN_PS_READY    1
#define TERN_PS_SLEEPING 2
#define TERN_PS_BLOCKED  3
#define TERN_PS_EXITED   4
struct tern_procinfo {
    int32_t pid, ppid;
    uint32_t state;          /* TERN_PS_* */
    uint32_t reserved;
    uint64_t ticks;          /* timer ticks spent running */
    uint64_t mem_pages;      /* user heap pages (brk) */
    char name[24];
    int32_t pgid;            /* process group (Day 10 session) */
    uint32_t reserved2;
};
#endif
#endif
