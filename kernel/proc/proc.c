/* User processes: address space setup, ring-3 entry, fault handling. */
#include "proc.h"
#include "fbdev.h"
#include "sched.h"
#include "vmm.h"
#include "pmm.h"
#include "mm.h"
#include "heap.h"
#include "kernel.h"
#include "kprintf.h"
#include "string.h"
#include "vfs.h"
#include "syscall_nr.h"
#include "tty.h"
#include "gdt.h"

extern __attribute__((noreturn)) void enter_user(uint64_t rip, uint64_t rsp);

static int next_pid = 1;
static unsigned live_procs;

/* Orphans that have exited, waiting for the reaper (linked through ->sibling). */
static struct process *orphans;
static struct waitq reaper_wq;
static struct process *all_procs;       /* interrupts off while walking or changing */

static void list_add(struct process *p) {
    uint64_t f = irq_save();
    p->all_next = all_procs;
    all_procs = p;
    irq_restore(f);
}

static void list_remove(struct process *p) {
    uint64_t f = irq_save();
    for (struct process **pp = &all_procs; *pp; pp = &(*pp)->all_next)
        if (*pp == p) { *pp = p->all_next; break; }
    irq_restore(f);
}

unsigned proc_live_count(void) { return live_procs; }

struct process *proc_current(void) {
    struct thread *t = thread_current();
    return t ? t->proc : NULL;
}

static void user_thread_start(void *arg) {
    struct process *p = arg;
    /* The scheduler already loaded p->pml4 into CR3 when it switched to us. */
    proc_check_kill();                  /* killed before it ever ran */
    enter_user(p->entry, p->stack_top);
}

/* Copy len bytes into the (already mapped) user range at va of another address space. */
void proc_copy_to_space(uint64_t pml4, uint64_t va, const void *src, size_t len) {
    const uint8_t *s = src;
    while (len) {
        uint64_t pa;
        kassert(vmm_translate(pml4, va, &pa, NULL));
        size_t chunk = PAGE_SIZE - (va & (PAGE_SIZE - 1));
        if (chunk > len) chunk = len;
        memcpy(phys_to_virt(pa), s, chunk);
        va += chunk; s += chunk; len -= chunk;
    }
}

/* Standard files and cwd for a new process (called before its thread exists).
 * files[i] (if given and not NULL) becomes fd i, otherwise fd i is the console. */
static int proc_setup_files(struct process *p, struct file *const *files) {
    struct process *parent = proc_current();
    p->cwd = parent && parent->cwd ? parent->cwd : vfs_root();
    vfs_hold(p->cwd);
    struct file *con = NULL;
    for (int i = 0; i < 3; i++) {
        struct file *f = files ? files[i] : NULL;
        if (!f) {
            if (!con) {
                int rc = vfs_open(NULL, "/dev/console", O_RDWR, &con);
                if (rc) return rc;
            } else {
                file_hold(con);
            }
            f = con;
        } else {
            file_hold(f);
        }
        p->fds[i] = f;
    }
    return 0;
}

static void proc_release_files(struct process *p) {
    for (int i = 0; i < PROC_MAX_FDS; i++)
        if (p->fds[i]) { file_close(p->fds[i]); p->fds[i] = NULL; }
    if (p->cwd) { vfs_put(p->cwd); p->cwd = NULL; }
}

static struct process *proc_alloc(const char *name, struct file *const *files) {
    struct process *p = kzalloc(sizeof *p);
    if (!p) return NULL;
    waitq_init(&p->child_wq);
    for (unsigned i = 0; i < sizeof p->name - 1 && name[i]; i++) p->name[i] = name[i];
    p->pml4 = vmm_new_space();
    if (!p->pml4 || proc_setup_files(p, files)) {
        proc_release_files(p);
        if (p->pml4) vmm_destroy_space(p->pml4);
        kfree(p);
        return NULL;
    }
    live_procs++;
    list_add(p);
    return p;
}

static void proc_free_failed(struct process *p) {
    proc_release_files(p);
    if (p->pml4) vmm_destroy_space(p->pml4);
    list_remove(p);
    kfree(p);
    live_procs--;
}

static int map_stack(struct process *p) {
    p->stack_top = USER_STACK_TOP;
    return vmm_map_user_pages(p->pml4, USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE,
                              USER_STACK_PAGES, PTE_W | PTE_NX);
}

/* Give the process a thread and make it runnable. */
static int proc_start(struct process *p) {
    /* The new thread must not run before it knows its address space. */
    uint64_t f = irq_save();
    p->pid = next_pid++;
    p->pgid = p->pid;
    p->thread = thread_create(p->name, user_thread_start, p);
    if (!p->thread) { irq_restore(f); return -ENOMEM; }
    p->thread->proc = p;
    p->thread->cr3 = p->pml4;
    /* the caller's process (NULL for kernel threads) becomes the parent */
    p->parent = proc_current();
    if (p->parent) {
        p->sibling = p->parent->children;
        p->parent->children = p;
    }
    irq_restore(f);
    return 0;
}

struct process *proc_spawn_blob(const char *name, const void *code, size_t len) {
    struct process *p = proc_alloc(name, NULL);
    if (!p) return NULL;
    uint64_t code_pages = ALIGN_UP(len, PAGE_SIZE) / PAGE_SIZE;
    /* code: user, read + execute (no PTE_W, no NX) */
    if (vmm_map_user_pages(p->pml4, USER_CODE_BASE, code_pages, 0)) goto fail;
    proc_copy_to_space(p->pml4, USER_CODE_BASE, code, len);
    if (map_stack(p)) goto fail;
    p->entry = USER_CODE_BASE;
    if (proc_start(p)) goto fail;
    return p;
fail:
    proc_free_failed(p);
    return NULL;
}

#define ARGV_MAX     32
#define ARGV_BYTES   2048

/* Initial user stack (System V x86-64 process entry): rsp -> argc, argv[0..argc-1],
 * NULL, envp NULL, auxv AT_NULL pair; strings above. rsp is 16-byte aligned. */
static int push_argv(uint64_t pml4, uint64_t *stack_top, const char *const *argv) {
    int argc = 0;
    size_t strbytes = 0;
    while (argv && argv[argc]) {
        if (argc == ARGV_MAX) return -EINVAL;
        strbytes += strlen(argv[argc]) + 1;
        argc++;
    }
    if (strbytes > ARGV_BYTES) return -EINVAL;
    size_t words = 1 + (size_t)argc + 1 + 1 + 2;
    size_t str_area = ALIGN_UP(strbytes, 16);
    size_t total = str_area + ALIGN_UP(words * 8, 16);
    uint8_t *buf = kzalloc(total);
    if (!buf) return -ENOMEM;
    uint64_t base = *stack_top - total;            /* user address of buf[0] */
    uint64_t *w = (uint64_t *)buf;
    uint64_t str_va = base + total - str_area;
    char *str = (char *)buf + total - str_area;
    w[0] = (uint64_t)argc;
    for (int i = 0; i < argc; i++) {
        size_t n = strlen(argv[i]) + 1;
        memcpy(str, argv[i], n);
        w[1 + i] = str_va;
        str += n; str_va += n;
    }
    /* w[argc+1] = NULL, envp NULL, auxv {0, 0}: already zero */
    proc_copy_to_space(pml4, base, buf, total);
    kfree(buf);
    *stack_top = base;
    return 0;
}

struct process *proc_spawn_elf(const char *path, const char *const *argv, int *err) {
    return proc_spawn_elf_fds(path, argv, NULL, err);
}

/* Read the executable at path (relative to the caller's cwd) into a kernel buffer. */
static int read_image(const char *path, uint8_t **img_out, size_t *len_out) {
    struct process *cur = proc_current();
    struct file *f;
    int rc = vfs_open(cur ? cur->cwd : NULL, path, O_RDONLY, &f);
    if (rc) return rc;
    struct tern_stat st;
    vfs_stat(f->node, &st);
    if (st.type != TERN_FT_FILE) { file_close(f); return -EACCES; }
    /* Read a private copy: the file may be written while we load it. */
    uint8_t *img = st.size ? kmalloc(st.size) : NULL;
    long n = img ? file_read(f, img, st.size) : -ENOEXEC;
    file_close(f);
    if (n != (long)st.size) { kfree(img); return n < 0 ? (int)n : -EIO; }
    *img_out = img;
    *len_out = st.size;
    return 0;
}

static const char *base_name(const char *path) {
    const char *name = path;
    for (const char *q = path; *q; q++) if (*q == '/' && q[1]) name = q + 1;
    return name;
}

struct process *proc_spawn_elf_fds(const char *path, const char *const *argv,
                                   struct file *const *files, int *err) {
    uint8_t *img;
    size_t len;
    int rc = read_image(path, &img, &len);
    if (rc) { *err = rc; return NULL; }
    if (live_procs >= PROC_MAX_LIVE) { kfree(img); *err = -EAGAIN; return NULL; }
    struct process *p = proc_alloc(base_name(path), files);
    if (!p) { kfree(img); *err = -ENOMEM; return NULL; }
    uint64_t brk;
    rc = elf_load(p->pml4, img, len, &p->entry, &brk);
    p->brk_base = p->brk = brk;
    kfree(img);
    if (!rc) rc = map_stack(p) ? -ENOMEM : 0;
    if (!rc) rc = push_argv(p->pml4, &p->stack_top, argv);
    if (!rc) rc = proc_start(p);
    if (rc) { proc_free_failed(p); *err = rc; return NULL; }
    return p;
}

/* exec (Day 12 session): build the new image in a fresh address space; only once that
 * has fully worked is the old one dropped, so a failed exec returns to the caller
 * untouched. pid, parent, children, pgid, fds and cwd stay. */
int proc_exec(const char *path, const char *const *argv, struct regs *r) {
    struct process *p = proc_current();
    kassert(p);
    uint8_t *img;
    size_t len;
    int rc = read_image(path, &img, &len);
    if (rc) return rc;
    uint64_t pml4 = vmm_new_space();
    if (!pml4) { kfree(img); return -ENOMEM; }
    uint64_t entry, brk, stack_top = USER_STACK_TOP;
    rc = elf_load(pml4, img, len, &entry, &brk);
    kfree(img);
    if (!rc && vmm_map_user_pages(pml4, USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE,
                                  USER_STACK_PAGES, PTE_W | PTE_NX))
        rc = -ENOMEM;
    if (!rc) rc = push_argv(pml4, &stack_top, argv);
    if (rc) { vmm_destroy_space(pml4); return rc; }

    fbdev_release_by(p->pid);           /* the new program knows nothing of the screen */
    uint64_t f = irq_save();
    uint64_t old = p->pml4;
    p->pml4 = pml4;
    p->thread->cr3 = pml4;
    __asm__ volatile("mov %0, %%cr3" : : "r"(pml4) : "memory");
    memset(p->name, 0, sizeof p->name);
    const char *name = base_name(path);
    for (unsigned i = 0; i < sizeof p->name - 1 && name[i]; i++) p->name[i] = name[i];
    p->entry = entry;
    p->stack_top = stack_top;
    p->brk_base = p->brk = brk;
    sched_fpu_reset_current();
    irq_restore(f);
    vmm_destroy_space(old);
    p->execs++;
    /* return to the new entry point with a clean register file (rax = 0) */
    memset(r, 0, sizeof *r);
    r->vector = 0x100;
    r->rip = entry;
    r->rcx = entry;
    r->cs = GDT_UCODE | 3;
    r->ss = GDT_UDATA | 3;
    r->rsp = stack_top;
    r->rflags = 0x202;
    r->r11 = 0x202;
    return 0;
}

static void push_orphan(struct process *p) {
    p->sibling = orphans;
    orphans = p;
    waitq_wake_one(&reaper_wq);
}

__attribute__((noreturn)) void proc_exit(int code) {
    struct process *p = proc_current();
    kassert(p);
    irq_enable();                 /* closing files may take the VFS mutex */
    proc_release_files(p);
    fbdev_release_by(p->pid);     /* give the screen back to the console (Day 11) */
    irq_disable();
    /* Children outlive us as orphans; those that already exited go to the reaper now,
     * the others when they exit. */
    for (struct process *c = p->children, *next; c; c = next) {
        next = c->sibling;
        c->parent = NULL;
        c->orphan = true;
        if (c->exited) push_orphan(c);
    }
    p->children = NULL;
    p->exited = true;
    p->exit_code = code;
    if (p->parent) waitq_wake_all(&p->parent->child_wq);
    else if (p->orphan) push_orphan(p);
    thread_exit(code);            /* (kernel-spawned processes: the spawner waits) */
}

void proc_thread_reaped(struct thread *t) {
    struct process *p = t->proc;
    p->ticks_run = t->ticks_run;
    p->switches = t->switches;
    if (p->pml4) {
        vmm_destroy_space(p->pml4);
        p->pml4 = 0;
    }
}

int proc_wait(struct process *p, struct process *info) {
    int code = thread_join(p->thread);
    kassert(p->pml4 == 0);
    p->thread = NULL;
    if (info) *info = *p;
    list_remove(p);
    kfree(p);
    live_procs--;
    return code;
}

/* Frees orphans once they have exited. */
static void reaper_thread(void *arg) {
    (void)arg;
    for (;;) {
        uint64_t f = irq_save();
        while (!orphans) waitq_sleep(&reaper_wq);
        struct process *p = orphans;
        orphans = p->sibling;
        irq_restore(f);
        proc_wait(p, NULL);
    }
}

void proc_init(void) {
    waitq_init(&reaper_wq);
    struct thread *t = thread_create("reaper", reaper_thread, NULL);
    kassert(t);
}

int proc_spawn_child(const char *path, const char *const *argv, struct file *const *files) {
    int err = 0;
    struct process *c = proc_spawn_elf_fds(path, argv, files, &err);
    return c ? c->pid : err;
}

int proc_wait_child(int pid, int *code, bool nohang) {
    struct process *p = proc_current();
    kassert(p);
    uint64_t f = irq_save();
    struct process *found;
    for (;;) {
        bool any = false;
        found = NULL;
        for (struct process *c = p->children; c; c = c->sibling) {
            if (pid != -1 && c->pid != pid) continue;
            any = true;
            if (c->exited) { found = c; break; }
        }
        if (found || !any) break;
        if (nohang) { irq_restore(f); return 0; }
        if (p->kill_code) { irq_restore(f); return -EINTR; }
        waitq_sleep(&p->child_wq);
    }
    if (!found) { irq_restore(f); return -ECHILD; }
    for (struct process **pp = &p->children; *pp; pp = &(*pp)->sibling)
        if (*pp == found) { *pp = found->sibling; break; }
    irq_restore(f);
    int cpid = found->pid;
    *code = proc_wait(found, NULL);   /* the thread may still be finishing: joins it */
    return cpid;
}

/* ---- demand paging (Day 20) ---- */
static uint64_t demand_faults;
uint64_t proc_demand_faults(void) { return demand_faults; }

bool proc_demand_fault(uint64_t va) {
    struct process *p = proc_current();
    if (!p || p->exited) return false;
    uint64_t page = ALIGN_DOWN(va, PAGE_SIZE);
    bool heap = p->brk_base && page >= p->brk_base && page < ALIGN_UP(p->brk, PAGE_SIZE);
    bool stack = page >= USER_STACK_TOP - USER_STACK_MAX && page < USER_STACK_TOP;
    if (!heap && !stack) return false;
    if (vmm_translate(p->pml4, page, NULL, NULL)) return false;   /* present: real fault */
    if (vmm_map_user_pages(p->pml4, page, 1, PTE_W | PTE_NX)) return false;  /* no memory */
    demand_faults++;
    return true;
}

__attribute__((noreturn)) void proc_user_fault(struct regs *r) {
    struct process *p = proc_current();
    if (!p) panic("ring-3 exception %lu without a process", r->vector);
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    p->faulted = true;
    p->fault_regs = *r;
    p->fault_cr2 = cr2;
    kprintf("user fault: pid %d '%s': %s (vector %lu, error %lx) at rip %lx",
            p->pid, p->name, exception_name(r->vector), r->vector, r->error, r->rip);
    if (r->vector == 14) kprintf(" cr2 %lx", cr2);
    if (r->vector == 14 && ALIGN_DOWN(cr2, PAGE_SIZE) == USER_STACK_GUARD)
        kprintf(" (stack overflow: guard page)");
    kprintf(" -> killed\n");
    proc_exit(PROC_FAULT_EXIT_BASE + (int)r->vector);
}

/* ---- kill (Day 9 session) ---- */
int proc_kill(int pid, int code) {
    uint64_t f = irq_save();
    struct process *p = all_procs;
    while (p && (p->pid != pid || !p->thread)) p = p->all_next;
    if (!p || p->exited) { irq_restore(f); return -ESRCH; }
    if (!p->kill_code) p->kill_code = code;
    /* Interrupt what it may be blocked in: a sleep, wait() for a child, a tty read.
     * Those loops re-check their condition, so the extra wakeups are harmless. */
    sched_wake_sleeper(p->thread);
    sched_wake_blocked(p->thread);    /* pipes */
    waitq_wake_all(&p->child_wq);
    tty_input_ready();
    irq_restore(f);
    return 0;
}

int proc_kill_group(int pgid, int code) {
    int n = 0;
    uint64_t f = irq_save();
    for (struct process *p = all_procs; p; p = p->all_next)
        if (p->pgid == pgid && p->thread && !p->exited && proc_kill(p->pid, code) == 0) n++;
    irq_restore(f);
    return n;
}

int proc_setpgid(int pid, int pgid) {
    struct process *self = proc_current();
    if (pid == 0) pid = self->pid;
    if (pgid == 0) pgid = pid;
    if (pgid < 0) return -EINVAL;
    uint64_t f = irq_save();
    struct process *t = NULL;
    if (pid == self->pid) t = self;
    else
        for (struct process *c = self->children; c; c = c->sibling)
            if (c->pid == pid) { t = c; break; }
    int rc = -ESRCH;
    if (t && !t->exited) { t->pgid = pgid; rc = 0; }
    irq_restore(f);
    return rc;
}

int proc_getpgid(int pid) {
    uint64_t f = irq_save();
    struct process *p = pid ? all_procs : proc_current();
    while (pid && p && p->pid != pid) p = p->all_next;
    int rc = p ? p->pgid : -ESRCH;
    irq_restore(f);
    return rc;
}

bool proc_kill_pending(void) {
    struct process *p = proc_current();
    return p && p->kill_code;
}

void proc_check_kill(void) {
    struct process *p = proc_current();
    if (p && p->kill_code && !p->exited) proc_exit(p->kill_code);
}

unsigned proc_list(struct tern_procinfo *out, unsigned max) {
    static const uint32_t st[] = { TERN_PS_READY, TERN_PS_RUNNING, TERN_PS_SLEEPING,
                                   TERN_PS_BLOCKED, TERN_PS_EXITED };
    unsigned n = 0;
    uint64_t f = irq_save();
    for (struct process *p = all_procs; p; p = p->all_next, n++) {
        if (n >= max) continue;
        struct tern_procinfo *o = &out[n];
        memset(o, 0, sizeof *o);
        o->pid = p->pid;
        o->ppid = p->parent ? p->parent->pid : 0;
        struct thread *t = p->thread;
        o->state = p->exited || !t ? TERN_PS_EXITED : st[t->state];
        o->ticks = t ? t->ticks_run : p->ticks_run;
        o->mem_pages = p->brk_base ? (ALIGN_UP(p->brk, PAGE_SIZE) - p->brk_base) / PAGE_SIZE : 0;
        memcpy(o->name, p->name, sizeof o->name);
        o->pgid = p->pgid;
    }
    irq_restore(f);
    return n;
}
