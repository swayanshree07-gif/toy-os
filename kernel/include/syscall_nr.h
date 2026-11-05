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
#define SYS_COUNT     16
#define USER_HEAP_MAX (64ull << 20)

#define ENOENT  2
#define EIO     5
#define EBADF   9
#define ENOMEM  12
#define EACCES  13
#define EFAULT  14
#define EEXIST  17
#define ENOTDIR 20
#define EISDIR  21
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
#endif
#endif
