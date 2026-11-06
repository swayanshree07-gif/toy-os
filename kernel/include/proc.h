#ifndef TERN_PROC_H
#define TERN_PROC_H
/* User processes (Day 15+): one address space + one thread running in ring 3.
 *
 * User virtual layout:
 *   USER_CODE_BASE (4 MiB)        program image (code read+execute, user)
 *   USER_STACK_TOP - 16 KiB ..    user stack (read/write, NX); nothing mapped above
 * A process ends through SYS_exit or by faulting in ring 3: the fault is reported and
 * the process is killed with exit code 128 + vector; the kernel carries on. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "idt.h"
#include "sched.h"

#define USER_CODE_BASE   0x0000000000400000ull
#define USER_STACK_TOP   0x00007FFFFFFFE000ull
#define USER_STACK_PAGES 4
#define PROC_FAULT_EXIT_BASE 128
#define PROC_MAX_FDS     16

struct thread;
struct file;
struct vnode;

struct process {
    int pid;
    char name[24];
    uint64_t pml4;                 /* physical; 0 once the address space is freed */
    struct thread *thread;
    uint64_t entry, stack_top;
    uint64_t brk_base;                /* end of the loaded image (ELF programs) */
    uint64_t brk;                     /* current break, brk_base <= brk */
    struct file *fds[PROC_MAX_FDS];   /* 0/1/2 = /dev/console; closed at exit */
    struct vnode *cwd;                /* held; relative paths start here */
    /* Process tree (Day 21). Links are changed with interrupts off. A process spawned
     * by a kernel thread has no parent and is waited for by that thread (proc_wait).
     * When a parent exits first, its children become orphans and the reaper thread
     * frees them once they have exited. */
    struct process *parent;
    struct process *children, *sibling;
    struct waitq child_wq;            /* parent sleeps here in wait() */
    bool exited;                      /* proc_exit ran; the thread is about to die */
    bool orphan;
    int exit_code;
    /* filled in when the thread dies */
    bool faulted;
    struct regs fault_regs;        /* user context at the fault */
    uint64_t fault_cr2;
    uint64_t ticks_run, switches;
    uint64_t user_irqs;            /* interrupts that arrived while in ring 3 */
};

/* Create a process running a flat position-independent code blob (copied to
 * USER_CODE_BASE). The thread is runnable when this returns. NULL if out of memory. */
struct process *proc_spawn_blob(const char *name, const void *code, size_t len);
/* Load an ELF64 executable from the VFS and start it with argv (NULL-terminated, may be
 * NULL). Returns NULL with *err set (negative errno) on failure. */
struct process *proc_spawn_elf(const char *path, const char *const *argv, int *err);
/* Validate and load an ELF64 image into the address space pml4; sets *entry and *brk
 * (first page after the highest segment). 0 or negative errno. */
int elf_load(uint64_t pml4, const void *image, size_t len, uint64_t *entry, uint64_t *brk);
/* Copy len bytes into mapped user memory of another address space. */
void proc_copy_to_space(uint64_t pml4, uint64_t va, const void *src, size_t len);
/* End the current process: closes its files, then the thread exits with code. */
__attribute__((noreturn)) void proc_exit(int code);
/* Wait for the process to end, free it and return its exit code. If info is not
 * NULL, the final process record (fault info, run statistics) is copied there. */
int proc_wait(struct process *p, struct process *info);
struct process *proc_current(void);
void proc_init(void);              /* starts the orphan reaper thread */
/* Spawn an ELF program as a child of the calling process (SYS_spawn): pid or -errno. */
int proc_spawn_child(const char *path, const char *const *argv);
/* Wait for a child (pid, or -1 = any) of the calling process to exit, free it and
 * store its exit code. Returns the child's pid, or -ECHILD. */
int proc_wait_child(int pid, int *code);
/* Processes allocated and not yet freed (for leak checks). */
unsigned proc_live_count(void);

/* Scheduler hook: the process's thread is dead and no longer running on its address
 * space; frees the user address space. */
void proc_thread_reaped(struct thread *t);
/* Exception raised in ring 3 with no handler: report and kill the process. */
__attribute__((noreturn)) void proc_user_fault(struct regs *r);

void syscall_init(void);           /* EFER.SCE, STAR, LSTAR, SFMASK */
void test_user(void);
void test_elf(void);
void test_wait(void);
void test_tty(void);
void test_shell(bool interactive);

#endif
