#ifndef TERN_SCHED_H
#define TERN_SCHED_H
/* Kernel threads and the preemptive round-robin scheduler (single CPU).
 *
 * Each thread has its own kernel stack in the KMAP area with an unmapped guard page
 * below it. Mutual exclusion inside the scheduler is "interrupts off" (one CPU).
 * The PIT tick drives preemption: a thread that has run for SCHED_QUANTUM ticks is
 * moved to the back of the run queue. Threads that are not ready are on exactly one
 * of: a wait queue, the sleep list, or nothing (dead). */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define SCHED_QUANTUM   2          /* ticks (20 ms at 100 Hz) */
#define KSTACK_PAGES    4          /* 16 KiB kernel stacks (+1 guard page) */

enum thread_state { T_READY, T_RUNNING, T_SLEEPING, T_BLOCKED, T_DEAD };

struct thread {
    uint64_t rsp;                  /* saved stack pointer (must stay first: switch.S) */
    int tid;
    enum thread_state state;
    char name[24];
    uint64_t stack_base, stack_top;   /* mapped stack [base, top); guard page below base */
    struct thread *next;           /* run queue / wait queue / sleep list link */
    struct thread *all_next;       /* list of all threads */
    uint64_t wake_tick;
    int exit_code;
    uint64_t ticks_run;            /* timer ticks spent running */
    uint64_t switches;             /* times switched in */
    struct waitq { struct thread *head, *tail; } joiners;
    bool boot_stack;               /* main thread: stack not ours to free */
    uint64_t cr3;                  /* address space (kernel PML4 for kernel threads) */
    struct process *proc;          /* owning user process, NULL for kernel threads */
    uint8_t *fpu;                  /* 512-byte fxsave area (16-aligned), user threads */
    struct waitq *intr_q;          /* queue of an interruptible sleep (waitq_sleep_intr) */
};

struct process;
typedef void (*thread_fn)(void *arg);

/* FPU/SSE: enabled at sched_init (CR0.MP, CR4.OSFXSR/OSXMMEXCPT). The kernel itself
 * is built without x87/SSE, so only user threads have FPU state: it is saved with
 * fxsave when one is switched out and restored when one is switched in. New user
 * threads start from the clean state captured after fninit (MXCSR 0x1f80). */
void sched_init(void);             /* adopt the running boot context as thread "main" */
bool sched_active(void);
struct thread *thread_create(const char *name, thread_fn fn, void *arg);
struct thread *thread_current(void);
void thread_yield(void);
__attribute__((noreturn)) void thread_exit(int code);
int thread_join(struct thread *t);  /* waits for exit, frees the thread, returns its code */
void thread_sleep_ms(uint64_t ms);
/* Wake t early if it is in thread_sleep_ms (true if it was). */
bool sched_wake_sleeper(struct thread *t);
void sched_set_preempt(bool on);

/* Called from the timer IRQ with the new tick count, and at the end of every IRQ. */
void sched_tick(uint64_t now);
void sched_irq_exit(void);

/* Wait queues. Callers disable interrupts (irq_save) around the condition check and
 * waitq_sleep; waitq_sleep returns with interrupts still disabled. */
void waitq_init(struct waitq *q);
void waitq_sleep(struct waitq *q);
bool waitq_wake_one(struct waitq *q);
unsigned waitq_wake_all(struct waitq *q);
/* Like waitq_sleep, but sched_wake_blocked(t) may take the thread off the queue early
 * (used by kill). Callers must re-check their condition in a loop. */
void waitq_sleep_intr(struct waitq *q);
/* Wake t if it is in waitq_sleep_intr. Returns true if it was. */
bool sched_wake_blocked(struct thread *t);

struct sched_stats { uint64_t switches, preemptions, threads_created, stacks_live, fpu_saves; };
void sched_get_stats(struct sched_stats *s);
/* For the #DF handler: prints a diagnosis if cr2 is in the current thread's guard page. */
bool sched_check_stack_overflow(uint64_t cr2);
void sched_dump(void);

void test_sched(void);

#endif
