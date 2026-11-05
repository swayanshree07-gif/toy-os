/* System calls: `syscall`/`sysret` setup and the dispatcher (entry stub in usermode.S). */
#include "proc.h"
#include "syscall_nr.h"
#include "sched.h"
#include "vmm.h"
#include "gdt.h"
#include "timer.h"
#include "console.h"
#include "kernel.h"
#include "kprintf.h"
#include "string.h"

#define MSR_EFER   0xC0000080
#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_SFMASK 0xC0000084

extern char syscall_entry[];

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile("wrmsr" : : "a"((uint32_t)v), "d"((uint32_t)(v >> 32)), "c"(msr));
}

void syscall_init(void) {
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1);              /* SCE */
    /* syscall: CS = STAR[47:32], SS = +8.  sysret (64-bit): CS = STAR[63:48] + 16,
     * SS = STAR[63:48] + 8, both with RPL 3 -> 0x23 / 0x1b with our GDT. */
    wrmsr(MSR_STAR, ((uint64_t)GDT_KCODE << 32) | ((uint64_t)(GDT_UDATA - 8) << 48));
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    /* clear TF, IF, DF, IOPL, NT, AC on entry */
    wrmsr(MSR_SFMASK, 0x47700);
}

static long sys_write(uint64_t fd, uint64_t buf, uint64_t len) {
    if (fd != 1 && fd != 2) return -EBADF;
    struct process *p = proc_current();
    if (!vmm_user_range_ok(p->pml4, buf, len, false)) return -EFAULT;
    /* Validated against the current page tables; nothing can unmap them while we run
     * (single CPU, one thread per process). */
    char chunk[129];
    for (uint64_t done = 0; done < len;) {
        uint64_t n = len - done > 128 ? 128 : len - done;
        memcpy(chunk, (const void *)(buf + done), n);
        chunk[n] = 0;
        console_write(chunk);
        done += n;
    }
    return (long)len;
}

static long syscall_do(struct regs *r) {
    uint64_t a0 = r->rdi, a1 = r->rsi, a2 = r->rdx;
    switch (r->rax) {
    case SYS_exit:      thread_exit((int)a0);
    case SYS_write:     return sys_write(a0, a1, a2);
    case SYS_getpid:    return proc_current()->pid;
    case SYS_yield:     thread_yield(); return 0;
    case SYS_sleep_ms:  thread_sleep_ms(a0 > 3600000 ? 3600000 : a0); return 0;
    case SYS_uptime_ms: return (long)timer_ms();
    default:            return -ENOSYS;
    }
}

/* Called by syscall_entry with IF=0 on the thread's kernel stack. */
void syscall_dispatch(struct regs *r) {
    kassert(proc_current());
    irq_enable();                       /* syscalls may block and are preemptible */
    r->rax = (uint64_t)syscall_do(r);
    irq_disable();
    /* sysret with a non-canonical rip would #GP in ring 0 on Intel CPUs. */
    if (r->rip >= USER_TOP) {
        r->vector = 13;
        proc_user_fault(r);
    }
    r->rflags |= 0x200;
}
