#ifndef TERN_USER_H
#define TERN_USER_H
/* TernOS user library: system call wrappers, strings, printf. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include "syscall_nr.h"

static inline long tern_syscall3(long nr, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(nr), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}
#define SC0(n)       tern_syscall3((n), 0, 0, 0)
#define SC1(n,a)     tern_syscall3((n), (long)(a), 0, 0)
#define SC2(n,a,b)   tern_syscall3((n), (long)(a), (long)(b), 0)
#define SC3(n,a,b,c) tern_syscall3((n), (long)(a), (long)(b), (long)(c))

/* Raw system calls: return the kernel's result (negative errno on error). */
__attribute__((noreturn)) static inline void exit(int code) { SC1(SYS_exit, code); __builtin_unreachable(); }
static inline long write(int fd, const void *b, size_t n) { return SC3(SYS_write, fd, b, n); }
static inline long read(int fd, void *b, size_t n)        { return SC3(SYS_read, fd, b, n); }
static inline int open(const char *p, int flags)          { return (int)SC2(SYS_open, p, flags); }
static inline int close(int fd)                           { return (int)SC1(SYS_close, fd); }
static inline long lseek(int fd, long off, int whence)    { return SC3(SYS_lseek, fd, off, whence); }
static inline int readdir(int fd, struct tern_dirent *d)  { return (int)SC2(SYS_readdir, fd, d); }
static inline int mkdir(const char *p)                    { return (int)SC1(SYS_mkdir, p); }
static inline int unlink(const char *p)                   { return (int)SC1(SYS_unlink, p); }
static inline int stat(const char *p, struct tern_stat *s){ return (int)SC2(SYS_stat, p, s); }
static inline int fstat(int fd, struct tern_stat *s)      { return (int)SC2(SYS_fstat, fd, s); }
static inline int getpid(void)                            { return (int)SC0(SYS_getpid); }
static inline void yield(void)                            { SC0(SYS_yield); }
static inline void sleep_ms(uint64_t ms)                  { SC1(SYS_sleep_ms, ms); }
static inline uint64_t uptime_ms(void)                    { return (uint64_t)SC0(SYS_uptime_ms); }
static inline long brk(uintptr_t addr)                    { return SC1(SYS_brk, addr); }
/* Day 21: processes */
static inline int spawn(const char *path, const char *const *argv) { return (int)SC3(SYS_spawn, path, argv, 0); }
/* fds[3]: our fd to give the child as its fd 0/1/2, or -1 for the console */
static inline int spawn_fds(const char *path, const char *const *argv, const int *fds) { return (int)SC3(SYS_spawn, path, argv, fds); }
static inline int wait(int pid, int *status)              { return (int)SC2(SYS_wait, pid, status); }
static inline int getppid(void)                           { return (int)SC0(SYS_getppid); }
static inline int chdir(const char *p)                    { return (int)SC1(SYS_chdir, p); }
static inline long getcwd(char *buf, size_t n)            { return SC2(SYS_getcwd, buf, n); }
static inline int kill(int pid)                           { return (int)SC1(SYS_kill, pid); }
static inline int setfg(int pid)                          { return (int)SC1(SYS_setfg, pid); }
static inline int sysinfo(struct tern_sysinfo *si)        { return (int)SC1(SYS_sysinfo, si); }
static inline int proclist(struct tern_procinfo *b, unsigned max) { return (int)SC2(SYS_proclist, b, max); }
static inline int pipe(int fds[2])                        { return (int)SC1(SYS_pipe, fds); }
static inline int setpgid(int pid, int pgid)              { return (int)SC2(SYS_setpgid, pid, pgid); }
static inline int getpgid(int pid)                        { return (int)SC1(SYS_getpgid, pid); }
static inline int rename(const char *a, const char *b)    { return (int)SC2(SYS_rename, a, b); }
/* Day 11 session: framebuffer and power */
static inline int fbinfo(struct tern_fbinfo *fi)          { return (int)SC1(SYS_fbinfo, fi); }
static inline int fbacquire(int on)                       { return (int)SC1(SYS_fbacquire, on); }
static inline int fbblit(const struct tern_fbblit *b)     { return (int)SC1(SYS_fbblit, b); }
static inline int poweroff(void)                          { return (int)SC0(SYS_poweroff); }
/* write all of buf (pipes may take it in pieces); bytes written or the error */
static inline long write_all(int fd, const void *b, size_t n) {
    size_t done = 0;
    while (done < n) {
        long r = write(fd, (const char *)b + done, n - done);
        if (r <= 0) return done ? (long)done : (r ? r : -EIO);
        done += (size_t)r;
    }
    return (long)done;
}

void *malloc(size_t n);
void free(void *p);
void *calloc(size_t n, size_t sz);
void *realloc(void *p, size_t n);
void malloc_stats(size_t *free_blocks, size_t *free_bytes);

void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
int atoi(const char *s);         /* optional '-', decimal digits */

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int dprintf(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int puts(const char *s);         /* appends a newline */

#endif
