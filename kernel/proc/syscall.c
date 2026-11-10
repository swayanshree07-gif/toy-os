/* System calls: `syscall`/`sysret` setup and the dispatcher (entry stub in usermode.S). */
#include "entry.h"
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
#include "heap.h"
#include "tty.h"
#include "fbdev.h"
#include "acpi.h"
#include "io.h"
#include "pci.h"
#include "ahci.h"

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
    /* Day 20: fault in lazy heap/stack pages first, as a user access would */
    if (len && va < 0x0000800000000000ull && len <= 0x0000800000000000ull - va)
        for (uint64_t pg = ALIGN_DOWN(va, PAGE_SIZE); pg < va + len; pg += PAGE_SIZE)
            if (!vmm_translate(proc_current()->pml4, pg, NULL, NULL) && !proc_demand_fault(pg))
                return false;              /* stop at the first hole: huge lengths are cheap */
    /* Day 22: the kernel is about to write: break copy-on-write sharing first */
    if (write && len && va < 0x0000800000000000ull && len <= 0x0000800000000000ull - va)
        for (uint64_t pg = ALIGN_DOWN(va, PAGE_SIZE); pg < va + len; pg += PAGE_SIZE)
            proc_cow_fault(pg);
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

static long sys_dup(uint64_t fd, uint64_t newfd, bool two) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    struct process *p = proc_current();
    if (two) {
        if (newfd >= PROC_MAX_FDS) return -EBADF;
        if (newfd == fd) return (long)fd;
    } else {
        newfd = 0;
        while (newfd < PROC_MAX_FDS && p->fds[newfd]) newfd++;
        if (newfd == PROC_MAX_FDS) return -EMFILE;
    }
    file_hold(f);
    struct file *old = p->fds[newfd];
    p->fds[newfd] = f;
    if (old) file_close(old);
    return (long)newfd;
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

static long sys_pipe(uint64_t ufds) {
    if (!user_ok(ufds, 2 * sizeof(int), true)) return -EFAULT;
    struct process *p = proc_current();
    int fd[2], n = 0;
    for (int i = 0; i < PROC_MAX_FDS && n < 2; i++)
        if (!p->fds[i]) fd[n++] = i;
    if (n < 2) return -EMFILE;
    struct file *r, *w;
    int rc = pipe_create(&r, &w);
    if (rc) return rc;
    p->fds[fd[0]] = r;
    p->fds[fd[1]] = w;
    ((int *)ufds)[0] = fd[0];
    ((int *)ufds)[1] = fd[1];
    return 0;
}

static long sys_rename(uint64_t ufrom, uint64_t uto) {
    char from[VFS_PATH_MAX], to[VFS_PATH_MAX];
    int rc = copy_user_str(from, ufrom, sizeof from);
    if (!rc) rc = copy_user_str(to, uto, sizeof to);
    if (rc) return rc;
    return vfs_rename(proc_current()->cwd, from, to);
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
        /* Day 20: nothing is mapped now; pages appear on first touch (proc_demand_fault).
         * Refuse growth that could never be backed by the free frames. */
        if ((new_end - old_end) / PAGE_SIZE > pmm_free_frames()) return (long)p->brk;
    } else {
        for (uint64_t va = new_end; va < old_end; va += PAGE_SIZE) {
            uint64_t pa;
            if (vmm_unmap(p->pml4, va, &pa)) pmm_free(pa);   /* vmm_unmap flushes the TLB */
        }
    }
    p->brk = want;
    return (long)want;
}

#define SPAWN_ARGV_MAX   32
#define SPAWN_ARGV_BYTES 2048

/* Copy a user argv array (NULL-terminated, may be NULL) into argv/strs (kernel). With
 * no arguments at all, argv[0] is the path, as a shell would pass it. */
static int copy_user_argv(uint64_t uargv, const char **argv, char *strs, const char *path) {
    size_t used = 0;
    int argc = 0;
    for (; uargv; argc++) {
        uint64_t slot = uargv + (uint64_t)argc * 8;
        if (slot < uargv || !user_ok(slot, 8, false)) return -EFAULT;
        uint64_t s = *(const uint64_t *)slot;
        if (!s) break;
        if (argc == SPAWN_ARGV_MAX || used == SPAWN_ARGV_BYTES) return -EINVAL;
        int rc = copy_user_str(strs + used, s, SPAWN_ARGV_BYTES - used);
        if (rc) return rc == -ENAMETOOLONG ? -EINVAL : rc;
        argv[argc] = strs + used;
        used += strlen(strs + used) + 1;
    }
    argv[argc] = NULL;
    if (argc == 0) { argv[0] = path; argv[1] = NULL; }
    return 0;
}

/* spawn(path, argv): argv strings are copied into one kernel buffer first. */
static long sys_spawn(uint64_t upath, uint64_t uargv, uint64_t ufds) {
    char path[VFS_PATH_MAX];
    int rc = copy_user_str(path, upath, sizeof path);
    if (rc) return rc;
    struct file *files[3] = { NULL, NULL, NULL };
    if (ufds) {
        if (!user_ok(ufds, 3 * sizeof(int), false)) return -EFAULT;
        for (int i = 0; i < 3; i++) {
            int fd = ((const int *)ufds)[i];
            if (fd == -1) continue;
            if (fd < 0 || !(files[i] = fd_get((uint64_t)fd))) return -EBADF;
        }
    }
    const char *argv[SPAWN_ARGV_MAX + 1];
    char *strs = kmalloc(SPAWN_ARGV_BYTES);
    if (!strs) return -ENOMEM;
    rc = copy_user_argv(uargv, argv, strs, path);
    /* (the files stay referenced by our fd table while the child takes its own refs) */
    if (!rc) rc = proc_spawn_child(path, argv, files);
    kfree(strs);
    return rc;
}

/* exec(path, argv): everything is copied out of the old image before it goes away. */
static long sys_exec(uint64_t upath, uint64_t uargv, struct regs *r) {
    char path[VFS_PATH_MAX];
    int rc = copy_user_str(path, upath, sizeof path);
    if (rc) return rc;
    const char *argv[SPAWN_ARGV_MAX + 1];
    char *strs = kmalloc(SPAWN_ARGV_BYTES);
    if (!strs) return -ENOMEM;
    rc = copy_user_argv(uargv, argv, strs, path);
    if (!rc) rc = proc_exec(path, argv, r);
    kfree(strs);
    return rc;
}

static long sys_wait(uint64_t pid, uint64_t ustatus, uint64_t flags) {
    if (flags & ~(uint64_t)TERN_WNOHANG) return -EINVAL;
    if (ustatus && !user_ok(ustatus, sizeof(int), true)) return -EFAULT;
    int code = 0;
    int rc = proc_wait_child((int)pid, &code, flags & TERN_WNOHANG);
    /* our own memory cannot have changed while we slept (one thread per process) */
    if (rc > 0 && ustatus) *(int *)ustatus = code;
    return rc;
}

static long sys_chdir(uint64_t upath) {
    char path[VFS_PATH_MAX];
    int rc = copy_user_str(path, upath, sizeof path);
    if (rc) return rc;
    struct process *p = proc_current();
    struct vnode *v = vfs_lookup(p->cwd, path, &rc);
    if (!v) return rc;
    if (v->type != VN_DIR) { vfs_put(v); return -ENOTDIR; }
    struct vnode *old = p->cwd;
    p->cwd = v;                       /* lookup's reference becomes the cwd's */
    vfs_put(old);
    return 0;
}

static long sys_getcwd(uint64_t ubuf, uint64_t size) {
    char path[VFS_PATH_MAX];
    int n = vfs_getpath(proc_current()->cwd, path, sizeof path);
    if (n < 0) return n;
    if ((uint64_t)n + 1 > size) return -ERANGE;
    if (!user_ok(ubuf, (uint64_t)n + 1, true)) return -EFAULT;
    memcpy((void *)ubuf, path, (size_t)n + 1);
    return n;
}

static long sys_statfs(uint64_t upath, uint64_t ust) {
    char path[VFS_PATH_MAX];
    int rc = copy_user_str(path, upath, sizeof path);
    if (rc) return rc;
    if (!user_ok(ust, sizeof(struct tern_statfs), true)) return -EFAULT;
    struct vnode *v = vfs_lookup(proc_current()->cwd, path, &rc);
    if (!v) return rc;
    struct tern_statfs st;
    vfs_statfs(v, &st);
    vfs_put(v);
    memcpy((void *)ust, &st, sizeof st);
    return 0;
}

static void copy_cstr(char *dst, const char *src, size_t size) {
    size_t n = 0;
    while (src && src[n] && n + 1 < size) { dst[n] = src[n]; n++; }
    dst[n] = 0;
}

static long sys_pcilist(uint64_t ubuf, uint64_t max) {
    int n = pci_count();
    uint64_t fill = (uint64_t)n < max ? (uint64_t)n : max;
    if (fill > 256) fill = 256;
    if (fill && !user_ok(ubuf, fill * sizeof(struct tern_pcidev), true)) return -EFAULT;
    for (uint64_t i = 0; i < fill; i++) {
        const struct pci_dev *d = pci_get((int)i);
        struct tern_pcidev o;
        memset(&o, 0, sizeof o);
        o.bus = d->bus; o.dev = d->dev; o.fn = d->fn;
        o.class_code = d->class_code; o.subclass = d->subclass; o.prog_if = d->prog_if;
        o.irq = d->irq_line; o.vendor = d->vendor; o.device = d->device;
        copy_cstr(o.kind, pci_class_name(d->class_code, d->subclass), sizeof o.kind);
        memcpy((struct tern_pcidev *)ubuf + i, &o, sizeof o);
    }
    return n;
}

static long sys_blklist(uint64_t ubuf, uint64_t max) {
    int n = ahci_disk_count();
    uint64_t fill = (uint64_t)n < max ? (uint64_t)n : max;
    if (fill > 64) fill = 64;
    if (fill && !user_ok(ubuf, fill * sizeof(struct tern_blkdev), true)) return -EFAULT;
    for (uint64_t i = 0; i < fill; i++) {
        const struct ahci_disk *d = ahci_disk_get((int)i);
        struct tern_blkdev o;
        memset(&o, 0, sizeof o);
        o.index = (uint32_t)i; o.port = (uint32_t)d->port; o.sectors = d->sectors;
        copy_cstr(o.model, d->model, sizeof o.model);
        copy_cstr(o.mount, fat_mount_of_disk((int)i), sizeof o.mount);
        memcpy((struct tern_blkdev *)ubuf + i, &o, sizeof o);
    }
    return n;
}

static long sys_sysinfo(uint64_t uinfo) {
    if (!user_ok(uinfo, sizeof(struct tern_sysinfo), true)) return -EFAULT;
    struct tern_sysinfo si;
    memset(&si, 0, sizeof si);
    si.uptime_ms = timer_ms();
    si.frames_total = pmm_total_frames();
    si.frames_free = pmm_free_frames();
    si.demand_faults = proc_demand_faults();
    si.user_pages = vmm_user_pages(proc_current()->pml4);
    const struct vmm_cow_stats *cs = vmm_cow_stats();
    si.forks = cs->forks;
    si.cow_shared = cs->shared_pages;
    si.cow_copies = cs->copies;
    si.cow_reuses = cs->reuses;
    si.frames_shared = pmm_shared_frames();
    struct heap_stats hs;
    heap_check(&hs);
    si.heap_mapped = hs.mapped;
    si.heap_used = hs.used_bytes;
    si.heap_free = hs.free_bytes;
    si.procs = proc_live_count();
    struct sched_stats ss;
    sched_get_stats(&ss);
    si.threads = ss.stacks_live;
    si.switches = ss.switches;
    si.preemptions = ss.preemptions;
    struct vfs_stats vs;
    vfs_get_stats(&vs);
    si.vfs_nodes = vs.nodes;
    si.vfs_bytes = vs.file_bytes;
    struct pipe_stats ps;
    pipe_get_stats(&ps);
    si.pipes_live = ps.live;
    si.pipes_created = ps.created;
    si.pipe_bytes = ps.bytes;
    memcpy((void *)uinfo, &si, sizeof si);
    return 0;
}

#define PROCLIST_MAX 64
static long sys_proclist(uint64_t ubuf, uint64_t max) {
    if (max > PROCLIST_MAX) max = PROCLIST_MAX;
    if (max && !user_ok(ubuf, max * sizeof(struct tern_procinfo), true)) return -EFAULT;
    struct tern_procinfo *tmp = max ? kmalloc(max * sizeof *tmp) : NULL;
    if (max && !tmp) return -ENOMEM;
    unsigned n = proc_list(tmp, (unsigned)max);
    if (max) memcpy((void *)ubuf, tmp, (n < max ? n : max) * sizeof *tmp);
    kfree(tmp);
    return n;
}

/* ---- framebuffer + power (Day 11 session) ---- */
static long sys_fbinfo(uint64_t uinfo) {
    if (!user_ok(uinfo, sizeof(struct tern_fbinfo), true)) return -EFAULT;
    struct tern_fbinfo fi;
    int rc = fbdev_info(&fi);
    if (rc) return rc;
    memcpy((void *)uinfo, &fi, sizeof fi);
    return 0;
}

static long sys_fbacquire(uint64_t on) {
    int pid = proc_current()->pid;
    if (on == 1) return fbdev_acquire(pid);
    if (on == 0) return fbdev_release_by(pid);
    return -EINVAL;
}

static long sys_fbblit(uint64_t ub) {
    if (!user_ok(ub, sizeof(struct tern_fbblit), false)) return -EFAULT;
    struct tern_fbblit b;
    memcpy(&b, (const void *)ub, sizeof b);
    return fbdev_blit(proc_current()->pid, &b, proc_current()->pml4);
}

static long sys_poweroff(void) {
    struct acpi_s5 s;
    if (!acpi_s5_info(&s)) return -EIO;
    kprintf("poweroff: ACPI S5 (PM1a_CNT %x, SLP_TYP %u)\n", s.pm1a_cnt, s.slp_typa);
    uint64_t f = irq_save();
    if (!(inw(s.pm1a_cnt) & 1) && s.smi_cmd && s.acpi_enable) {   /* SCI_EN off: enable ACPI */
        outb(s.smi_cmd, s.acpi_enable);
        for (int i = 0; i < 1000000 && !(inw(s.pm1a_cnt) & 1); i++) __asm__ volatile("pause");
    }
    outw(s.pm1a_cnt, (uint16_t)((s.slp_typa << 10) | (1u << 13)));
    if (s.pm1b_cnt) outw(s.pm1b_cnt, (uint16_t)((s.slp_typb << 10) | (1u << 13)));
    for (int i = 0; i < 10000000; i++) __asm__ volatile("pause");
    irq_restore(f);
    return -EIO;                         /* still running: the firmware ignored us */
}

static long syscall_do(struct regs *r) {
    uint64_t a0 = r->rdi, a1 = r->rsi, a2 = r->rdx;
    switch (r->rax) {
    case SYS_exit:      proc_exit((int)a0);
    case SYS_write:     return sys_write(a0, a1, a2);
    case SYS_getpid:    return proc_current()->pid;
    case SYS_yield:     thread_yield(); return 0;
    case SYS_sleep_ms:
        if (a0 == 0) thread_yield();    /* (a 0-tick sleep would wait for the next tick) */
        else {
            /* check-then-sleep with interrupts off, so a kill can't slip in between
             * (proc_kill wakes sleepers that are already on the sleep list) */
            uint64_t f = irq_save();
            if (!proc_kill_pending()) thread_sleep_ms(a0 > 3600000 ? 3600000 : a0);
            irq_restore(f);
        }
        return 0;
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
    case SYS_mmap:      return proc_mmap(a0, a1);
    case SYS_munmap:    return proc_munmap(a0, a1);
    case SYS_spawn:     return sys_spawn(a0, a1, a2);
    case SYS_wait:      return sys_wait(a0, a1, a2);
    case SYS_getppid: {
        uint64_t f = irq_save();
        struct process *pp = proc_current()->parent;
        int ppid = pp ? pp->pid : 0;
        irq_restore(f);
        return ppid;
    }
    case SYS_chdir:     return sys_chdir(a0);
    case SYS_getcwd:    return sys_getcwd(a0, a1);
    case SYS_kill:
        if ((int64_t)a0 <= 0 || a0 > 0x7fffffff) return -ESRCH;
        return proc_kill((int)a0, TERN_EXIT_KILLED);
    case SYS_setfg:     return tty_set_fg((int)a0);
    case SYS_sysinfo:   return sys_sysinfo(a0);
    case SYS_proclist:  return sys_proclist(a0, a1);
    case SYS_pipe:      return sys_pipe(a0);
    case SYS_setpgid:
        if ((int64_t)a0 < 0 || a0 > 0x7fffffff || a1 > 0x7fffffff) return -EINVAL;
        return proc_setpgid((int)a0, (int)a1);
    case SYS_getpgid:
        if ((int64_t)a0 < 0 || a0 > 0x7fffffff) return -ESRCH;
        return proc_getpgid((int)a0);
    case SYS_rename:    return sys_rename(a0, a1);
    case SYS_fbinfo:    return sys_fbinfo(a0);
    case SYS_fbacquire: return sys_fbacquire(a0);
    case SYS_fbblit:    return sys_fbblit(a0);
    case SYS_poweroff:  return sys_poweroff();
    case SYS_exec:      return sys_exec(a0, a1, r);
    case SYS_fork:      return proc_fork(r);
    case SYS_statfs:    return sys_statfs(a0, a1);
    case SYS_sync:      return vfs_sync();
    case SYS_pcilist:   return sys_pcilist(a0, a1);
    case SYS_blklist:   return sys_blklist(a0, a1);
    case SYS_dup:       return sys_dup(a0, 0, false);
    case SYS_dup2:      return sys_dup(a0, a1, true);
    default:            return -ENOSYS;
    }
}

/* Called by syscall_entry with IF=0 on the thread's kernel stack. */
void syscall_dispatch(struct regs *r) {
    kassert(proc_current());
    irq_enable();                       /* syscalls may block and are preemptible */
    proc_check_kill();                  /* a kill that arrived while we were in ring 3
                                           but before any interrupt: don't start a sleep */
    r->rax = (uint64_t)syscall_do(r);
    proc_check_kill();                  /* kill() / Ctrl-C: exit instead of returning */
    irq_disable();
    /* sysret with a non-canonical rip would #GP in ring 0 on Intel CPUs. */
    if (r->rip >= USER_TOP) {
        r->vector = 13;
        proc_user_fault(r);
    }
    r->rflags |= 0x200;
}
