/* System calls: `syscall`/`sysret` setup and the dispatcher (entry stub in usermode.S). */
#include "proc.h"
#include "syscall_nr.h"
#include "sched.h"
#include "vmm.h"
#include "gdt.h"
#include "timer.h"
#include "kernel.h"
#include "kprintf.h"
#include "string.h"
#include "vfs.h"
#include "mm.h"
#include "pmm.h"

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

/* ---- user memory helpers ----
 * User pointers are validated against the current page tables; nothing can unmap them
 * while we run (single CPU, one thread per process), so plain memcpy is then safe. */
static bool user_ok(uint64_t va, uint64_t len, bool write) {
    return vmm_user_range_ok(proc_current()->pml4, va, len, write);
}

/* Copy a NUL-terminated user string (at most max-1 chars) into dst. */
static int copy_user_str(char *dst, uint64_t src, size_t max) {
    for (size_t i = 0; i < max; i++) {
        uint64_t va = src + i;
        if ((i == 0 || (va & (PAGE_SIZE - 1)) == 0) && !user_ok(va, 1, false)) return -EFAULT;
        dst[i] = *(const char *)va;
        if (!dst[i]) return 0;
    }
    return -ENAMETOOLONG;
}

static struct file *fd_get(uint64_t fd) {
    if (fd >= PROC_MAX_FDS) return NULL;
    return proc_current()->fds[fd];
}

static long sys_write(uint64_t fd, uint64_t buf, uint64_t len) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    if (!user_ok(buf, len, false)) return -EFAULT;
    return file_write(f, (const void *)buf, len);
}

static long sys_read(uint64_t fd, uint64_t buf, uint64_t len) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    if (!user_ok(buf, len, true)) return -EFAULT;
    return file_read(f, (void *)buf, len);
}

static long sys_open(uint64_t upath, uint64_t flags) {
    char path[VFS_PATH_MAX];
    int rc = copy_user_str(path, upath, sizeof path);
    if (rc) return rc;
    struct process *p = proc_current();
    int fd = 0;
    while (fd < PROC_MAX_FDS && p->fds[fd]) fd++;
    if (fd == PROC_MAX_FDS) return -EMFILE;
    struct file *f;
    rc = vfs_open(p->cwd, path, (int)flags, &f);
    if (rc) return rc;
    p->fds[fd] = f;
    return fd;
}

static long sys_close(uint64_t fd) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    proc_current()->fds[fd] = NULL;
    file_close(f);
    return 0;
}

static long sys_lseek(uint64_t fd, uint64_t off, uint64_t whence) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    return file_seek(f, (int64_t)off, (int)whence);
}

static long sys_readdir(uint64_t fd, uint64_t ud) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    if (!user_ok(ud, sizeof(struct tern_dirent), true)) return -EFAULT;
    struct tern_dirent d;
    int rc = file_readdir(f, &d);
    if (rc == 1) memcpy((void *)ud, &d, sizeof d);
    return rc;
}

static long sys_path_op(uint64_t nr, uint64_t upath, uint64_t ust) {
    char path[VFS_PATH_MAX];
    int rc = copy_user_str(path, upath, sizeof path);
    if (rc) return rc;
    struct process *p = proc_current();
    if (nr == SYS_mkdir) return vfs_mkdir(p->cwd, path);
    if (nr == SYS_unlink) return vfs_unlink(p->cwd, path);
    /* SYS_stat */
    if (!user_ok(ust, sizeof(struct tern_stat), true)) return -EFAULT;
    struct vnode *v = vfs_lookup(p->cwd, path, &rc);
    if (!v) return rc;
    struct tern_stat st;
    vfs_stat(v, &st);
    vfs_put(v);
    memcpy((void *)ust, &st, sizeof st);
    return 0;
}

static long sys_fstat(uint64_t fd, uint64_t ust) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    if (!user_ok(ust, sizeof(struct tern_stat), true)) return -EFAULT;
    struct tern_stat st;
    vfs_stat(f->node, &st);
    memcpy((void *)ust, &st, sizeof st);
    return 0;
}

/* Move the program break. Pages between brk_base and the break are mapped read/write,
 * NX and zero-filled; shrinking unmaps and frees them. Returns the (possibly unchanged)
 * break, like Linux: callers compare it with what they asked for. */
static long sys_brk(uint64_t want) {
    struct process *p = proc_current();
    if (!p->brk_base) return -ENOSYS;                   /* flat blobs have no heap */
    if (want == 0 || want < p->brk_base || want - p->brk_base > USER_HEAP_MAX)
        return (long)p->brk;
    uint64_t old_end = ALIGN_UP(p->brk, PAGE_SIZE), new_end = ALIGN_UP(want, PAGE_SIZE);
    if (new_end > old_end) {
        if (vmm_map_user_pages(p->pml4, old_end, (new_end - old_end) / PAGE_SIZE,
                               PTE_W | PTE_NX)) {
            /* roll back whatever got mapped */
            for (uint64_t va = old_end; va < new_end; va += PAGE_SIZE) {
                uint64_t pa;
                if (vmm_unmap(p->pml4, va, &pa)) pmm_free(pa);
            }
            return (long)p->brk;
        }
    } else {
        for (uint64_t va = new_end; va < old_end; va += PAGE_SIZE) {
            uint64_t pa;
            if (vmm_unmap(p->pml4, va, &pa)) pmm_free(pa);   /* vmm_unmap flushes the TLB */
        }
    }
    p->brk = want;
    return (long)want;
}

static long syscall_do(struct regs *r) {
    uint64_t a0 = r->rdi, a1 = r->rsi, a2 = r->rdx;
    switch (r->rax) {
    case SYS_exit:      proc_exit((int)a0);
    case SYS_write:     return sys_write(a0, a1, a2);
    case SYS_getpid:    return proc_current()->pid;
    case SYS_yield:     thread_yield(); return 0;
    case SYS_sleep_ms:  thread_sleep_ms(a0 > 3600000 ? 3600000 : a0); return 0;
    case SYS_uptime_ms: return (long)timer_ms();
    case SYS_open:      return sys_open(a0, a1);
    case SYS_close:     return sys_close(a0);
    case SYS_read:      return sys_read(a0, a1, a2);
    case SYS_lseek:     return sys_lseek(a0, a1, a2);
    case SYS_readdir:   return sys_readdir(a0, a1);
    case SYS_mkdir:
    case SYS_unlink:
    case SYS_stat:      return sys_path_op(r->rax, a0, a1);
    case SYS_fstat:     return sys_fstat(a0, a1);
    case SYS_brk:       return sys_brk(a0);
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
