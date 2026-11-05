/* Kernel threads, context switching and the round-robin scheduler. */
#include "sched.h"
#include "mm.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "idt.h"
#include "gdt.h"
#include "timer.h"
#include "kernel.h"
#include "kprintf.h"
#include "string.h"
#include "proc.h"

extern void context_switch(uint64_t *save_rsp, uint64_t new_rsp);
extern char thread_entry[];

static struct thread *current, *idle_thread;
static struct thread *rq_head, *rq_tail;     /* ready threads, FIFO */
static struct thread *sleepers;              /* sorted by wake_tick */
static struct thread *all_threads;
static struct thread *dead_stack_owner;      /* stack to free after the next switch */
static bool active, preempt_on, need_resched;
static unsigned slice;
static int next_tid;
static struct sched_stats stats;

/* ---- kernel stacks: [guard page][KSTACK_PAGES pages] in the KMAP area ----
 * Virtual space is handed out by a bump pointer and never reused (64 GiB of KMAP
 * space is plenty); the frames are returned to the PMM on free. */
#define KSTACK_AREA (KMAP_BASE + (1ull << 30))   /* below 1 GiB is used by mm tests */
static uint64_t kstack_next = KSTACK_AREA;

static bool kstack_alloc(struct thread *t) {
    uint64_t base = kstack_next + PAGE_SIZE;      /* skip the guard page */
    for (unsigned i = 0; i < KSTACK_PAGES; i++) {
        uint64_t pa = pmm_alloc();
        if (!pa || vmm_map(vmm_kernel_pml4(), base + i * PAGE_SIZE, pa, VMM_KERNEL_RW) != 0) {
            if (pa) pmm_free(pa);
            for (unsigned j = 0; j < i; j++) {
                uint64_t old;
                if (vmm_unmap(vmm_kernel_pml4(), base + j * PAGE_SIZE, &old)) pmm_free(old);
            }
            return false;
        }
    }
    kstack_next = base + KSTACK_PAGES * PAGE_SIZE;
    t->stack_base = base;
    t->stack_top = base + KSTACK_PAGES * PAGE_SIZE;
    stats.stacks_live++;
    return true;
}

static void kstack_free(struct thread *t) {
    for (uint64_t va = t->stack_base; va < t->stack_top; va += PAGE_SIZE) {
        uint64_t old;
        if (vmm_unmap(vmm_kernel_pml4(), va, &old)) pmm_free(old);
    }
    t->stack_base = t->stack_top = 0;
    stats.stacks_live--;
}

/* ---- queues (interrupts must be off) ---- */
static void rq_push(struct thread *t) {
    t->state = T_READY;
    t->next = NULL;
    if (rq_tail) rq_tail->next = t; else rq_head = t;
    rq_tail = t;
}

static struct thread *rq_pop(void) {
    struct thread *t = rq_head;
    if (t) {
        rq_head = t->next;
        if (!rq_head) rq_tail = NULL;
        t->next = NULL;
    }
    return t;
}

static void make_ready(struct thread *t) {
    rq_push(t);
    if (current == idle_thread) need_resched = true;
}

/* Runs on the new thread's stack right after every switch. */
static void finish_switch(void) {
    if (dead_stack_owner) {
        /* we are on another thread's stack and address space now */
        if (dead_stack_owner->proc) proc_thread_reaped(dead_stack_owner);
        kstack_free(dead_stack_owner);
        dead_stack_owner = NULL;
    }
}

/* Pick the next thread and switch to it. Interrupts must be disabled. */
static void schedule(void) {
    struct thread *prev = current;
    need_resched = false;
    if (prev->state == T_RUNNING && prev != idle_thread) rq_push(prev);
    struct thread *next = rq_pop();
    if (!next) {
        if (prev == idle_thread) return;
        next = idle_thread;
    }
    next->state = T_RUNNING;
    slice = 0;
    if (next == prev) return;
    if (prev == idle_thread) prev->state = T_READY;
    if (prev->state == T_DEAD && !prev->boot_stack) dead_stack_owner = prev;
    current = next;
    next->switches++;
    stats.switches++;
    tss_set_kernel_stack(next->stack_top);
    if ((read_cr3() & PTE_ADDR) != next->cr3) write_cr3(next->cr3);
    context_switch(&prev->rsp, next->rsp);
    finish_switch();
}

__attribute__((noreturn)) void thread_bootstrap(thread_fn fn, void *arg) {
    finish_switch();
    irq_enable();                  /* we arrive here from schedule() with IRQs off */
    fn(arg);
    thread_exit(0);
}

static struct thread *thread_alloc(const char *name) {
    struct thread *t = kzalloc(sizeof *t);
    if (!t) return NULL;
    t->tid = next_tid++;
    for (unsigned i = 0; i < sizeof t->name - 1 && name[i]; i++) t->name[i] = name[i];
    waitq_init(&t->joiners);
    t->cr3 = vmm_kernel_pml4();
    return t;
}

static void idle_loop(void *arg) {
    (void)arg;
    for (;;) __asm__ volatile("sti; hlt" ::: "memory");
}

void sched_init(void) {
    uint64_t f = irq_save();
    struct thread *m = thread_alloc("main");
    kassert(m);
    m->state = T_RUNNING;
    m->boot_stack = true;
    m->all_next = all_threads;
    all_threads = m;
    current = m;
    stats.threads_created++;
    /* idle is a real thread but never sits on the run queue */
    idle_thread = thread_create("idle", idle_loop, NULL);
    kassert(idle_thread);
    rq_head = rq_tail = NULL;
    idle_thread->state = T_READY;
    active = true;
    irq_restore(f);
}

bool sched_active(void) { return active; }
struct thread *thread_current(void) { return current; }
void sched_set_preempt(bool on) { preempt_on = on; }

struct thread *thread_create(const char *name, thread_fn fn, void *arg) {
    struct thread *t = thread_alloc(name);
    if (!t) return NULL;
    if (!kstack_alloc(t)) { kfree(t); return NULL; }
    /* Initial frame popped by context_switch: r15 r14 r13 r12 rbx rbp, then the
     * return address thread_entry. After that ret, rsp == stack_top (16-aligned). */
    uint64_t *sp = (uint64_t *)t->stack_top;
    *--sp = (uint64_t)thread_entry;
    *--sp = 0;                     /* rbp */
    *--sp = 0;                     /* rbx */
    *--sp = (uint64_t)fn;          /* r12 */
    *--sp = (uint64_t)arg;         /* r13 */
    *--sp = 0;                     /* r14 */
    *--sp = 0;                     /* r15 */
    t->rsp = (uint64_t)sp;
    uint64_t f = irq_save();
    t->all_next = all_threads;
    all_threads = t;
    stats.threads_created++;
    if (idle_thread) rq_push(t);   /* (idle itself is created before idle_thread is set) */
    irq_restore(f);
    return t;
}

void thread_yield(void) {
    uint64_t f = irq_save();
    schedule();
    irq_restore(f);
}

__attribute__((noreturn)) void thread_exit(int code) {
    irq_disable();
    struct thread *t = current;
    kassert(t != idle_thread);
    t->exit_code = code;
    t->state = T_DEAD;
    waitq_wake_all(&t->joiners);
    schedule();
    panic("dead thread %d scheduled", t->tid);
}

int thread_join(struct thread *t) {
    kassert(t != current);
    uint64_t f = irq_save();
    while (t->state != T_DEAD) waitq_sleep(&t->joiners);
    int code = t->exit_code;
    for (struct thread **pp = &all_threads; *pp; pp = &(*pp)->all_next)
        if (*pp == t) { *pp = t->all_next; break; }
    irq_restore(f);
    kassert(t->stack_base == 0);   /* freed by finish_switch before we could run */
    kfree(t);
    return code;
}

void thread_sleep_ms(uint64_t ms) {
    uint64_t ticks = (ms * TIMER_HZ + 999) / 1000;
    uint64_t f = irq_save();
    struct thread *t = current;
    t->wake_tick = timer_ticks() + ticks;
    struct thread **pp = &sleepers;
    while (*pp && (*pp)->wake_tick <= t->wake_tick) pp = &(*pp)->next;
    t->next = *pp;
    *pp = t;
    t->state = T_SLEEPING;
    schedule();
    irq_restore(f);
}

void sched_tick(uint64_t now) {
    if (!active) return;
    current->ticks_run++;
    while (sleepers && sleepers->wake_tick <= now) {
        struct thread *t = sleepers;
        sleepers = t->next;
        make_ready(t);
    }
    if (current != idle_thread && preempt_on && ++slice >= SCHED_QUANTUM && rq_head) {
        stats.preemptions++;
        need_resched = true;
    }
}

void sched_irq_exit(void) {
    if (active && need_resched) schedule();
}

/* ---- wait queues ---- */
void waitq_init(struct waitq *q) { q->head = q->tail = NULL; }

void waitq_sleep(struct waitq *q) {
    struct thread *t = current;
    kassert(t != idle_thread);
    t->state = T_BLOCKED;
    t->next = NULL;
    if (q->tail) q->tail->next = t; else q->head = t;
    q->tail = t;
    schedule();
}

bool waitq_wake_one(struct waitq *q) {
    uint64_t f = irq_save();
    struct thread *t = q->head;
    if (t) {
        q->head = t->next;
        if (!q->head) q->tail = NULL;
        make_ready(t);
    }
    irq_restore(f);
    return t != NULL;
}

unsigned waitq_wake_all(struct waitq *q) {
    unsigned n = 0;
    while (waitq_wake_one(q)) n++;
    return n;
}

void sched_get_stats(struct sched_stats *s) {
    uint64_t f = irq_save();
    *s = stats;
    irq_restore(f);
}

bool sched_check_stack_overflow(uint64_t cr2) {
    struct thread *t = current;
    if (!t || t->boot_stack || !t->stack_base) return false;
    if (cr2 >= t->stack_base - PAGE_SIZE && cr2 < t->stack_base) {
        kprintf("kernel stack overflow in thread %d '%s' (guard page %lx hit)\n",
                t->tid, t->name, (unsigned long)(t->stack_base - PAGE_SIZE));
        return true;
    }
    return false;
}

void sched_dump(void) {
    static const char *const st[] = { "ready", "running", "sleeping", "blocked", "dead" };
    kprintf("  tid name             state     ticks  switches\n");
    for (struct thread *t = all_threads; t; t = t->all_next)
        kprintf("  %3d %-16s %-9s %6lu %8lu\n", t->tid, t->name, st[t->state],
                t->ticks_run, t->switches);
}
