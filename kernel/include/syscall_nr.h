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
#define SYS_COUNT     6

#define EBADF   9
#define EFAULT  14
#define EINVAL  22
#define ENOSYS  38
#endif
