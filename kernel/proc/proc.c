/* User processes: address space setup, ring-3 entry, fault handling. */
#include "proc.h"
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

extern __attribute__((noreturn)) void enter_user(uint64_t rip, uint64_t rsp);

static int next_pid = 1;
static unsigned live_procs;

/* Orphans that have exited, waiting for the reaper (linked through ->sibling). */
static struct process *orphans;
static struct waitq reaper_wq;

unsigned proc_live_count(void) { return live_procs; }

struct process *proc_current(void) {
    struct thread *t = thread_current();
    return t ? t->proc : NULL;
}

static void user_thread_start(void *arg) {
    struct process *p = arg;
    /* The scheduler already loaded p->pml4 into CR3 when it switched to us. */
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

/* Standard files and cwd for a new process (called before its thread exists). */
static int proc_setup_files(struct process *p) {
    struct process *parent = proc_current();
    p->cwd = parent && parent->cwd ? parent->cwd : vfs_root();
    vfs_hold(p->cwd);
    struct file *con;
    int rc = vfs_open(NULL, "/dev/console", O_RDWR, &con);
    if (rc) return rc;
    p->fds[0] = con;
    for (int i = 1; i < 3; i++) { file_hold(con); p->fds[i] = con; }
    return 0;
}

static void proc_release_files(struct process *p) {
    for (int i = 0; i < PROC_MAX_FDS; i++)
        if (p->fds[i]) { file_close(p->fds[i]); p->fds[i] = NULL; }
    if (p->cwd) { vfs_put(p->cwd); p->cwd = NULL; }
}

static struct process *proc_alloc(const char *name) {
    struct process *p = kzalloc(sizeof *p);
    if (!p) return NULL;
    waitq_init(&p->child_wq);
    for (unsigned i = 0; i < sizeof p->name - 1 && name[i]; i++) p->name[i] = name[i];
    p->pml4 = vmm_new_space();
    if (!p->pml4 || proc_setup_files(p)) {
        proc_release_files(p);
        if (p->pml4) vmm_destroy_space(p->pml4);
        kfree(p);
        return NULL;
    }
    live_procs++;
    return p;
}

static void proc_free_failed(struct process *p) {
    proc_release_files(p);
    if (p->pml4) vmm_destroy_space(p->pml4);
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
    struct process *p = proc_alloc(name);
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
static int push_argv(struct process *p, const char *const *argv) {
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
    uint64_t base = p->stack_top - total;          /* user address of buf[0] */
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
    proc_copy_to_space(p->pml4, base, buf, total);
    kfree(buf);
    p->stack_top = base;
    return 0;
}

struct process *proc_spawn_elf(const char *path, const char *const *argv, int *err) {
    struct process *cur = proc_current();
    struct file *f;
    int rc = vfs_open(cur ? cur->cwd : NULL, path, O_RDONLY, &f);
    if (rc) { *err = rc; return NULL; }
    struct tern_stat st;
    vfs_stat(f->node, &st);
    if (st.type != TERN_FT_FILE) { file_close(f); *err = -EACCES; return NULL; }
    /* Read a private copy: the file may be written while we load it. */
    uint8_t *img = st.size ? kmalloc(st.size) : NULL;
    long n = img ? file_read(f, img, st.size) : -ENOEXEC;
    file_close(f);
    if (n != (long)st.size) { kfree(img); *err = n < 0 ? (int)n : -EIO; return NULL; }

    const char *name = path;
    for (const char *q = path; *q; q++) if (*q == '/' && q[1]) name = q + 1;
    struct process *p = proc_alloc(name);
    if (!p) { kfree(img); *err = -ENOMEM; return NULL; }
    uint64_t brk;
    rc = elf_load(p->pml4, img, st.size, &p->entry, &brk);
    p->brk_base = p->brk = brk;
    kfree(img);
    if (!rc) rc = map_stack(p) ? -ENOMEM : 0;
    if (!rc) rc = push_argv(p, argv);
    if (!rc) rc = proc_start(p);
    if (rc) { proc_free_failed(p); *err = rc; return NULL; }
    return p;
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

int proc_spawn_child(const char *path, const char *const *argv) {
    int err = 0;
    struct process *c = proc_spawn_elf(path, argv, &err);
    return c ? c->pid : err;
}

int proc_wait_child(int pid, int *code) {
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
    kprintf(" -> killed\n");
    proc_exit(PROC_FAULT_EXIT_BASE + (int)r->vector);
}
