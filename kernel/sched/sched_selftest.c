/* Boot-time self-tests for threads, the scheduler and synchronisation. */
#include "sched.h"
#include "sync.h"
#include "pmm.h"
#include "timer.h"
#include "kernel.h"
#include "kprintf.h"
#include "string.h"

/* ---- Day 10: cooperative switching (preemption off) ---- */
static char trace[64];
static unsigned trace_len;

static void coop_thread(void *arg) {
    char letter = (char)(uintptr_t)arg;
    for (int i = 0; i < 3; i++) {
        trace[trace_len++] = letter;
        trace[trace_len++] = (char)('0' + i);
        thread_yield();
    }
    thread_exit(letter);
}

static void test_coop(void) {
    struct sched_stats s0, s1;
    sched_get_stats(&s0);
    uint64_t frames0 = pmm_free_frames();
    trace_len = 0;
    memset(trace, 0, sizeof trace);
    struct thread *a = thread_create("coop-A", coop_thread, (void *)(uintptr_t)'A');
    struct thread *b = thread_create("coop-B", coop_thread, (void *)(uintptr_t)'B');
    kassert(a && b);
    int ca = thread_join(a), cb = thread_join(b);
    if (strcmp(trace, "A0B0A1B1A2B2") || ca != 'A' || cb != 'B')
        panic("coop: trace '%s' exit codes %d %d", trace, ca, cb);
    sched_get_stats(&s1);
    if (s1.stacks_live != s0.stacks_live || pmm_free_frames() != frames0)
        panic("coop: stacks %lu->%lu frames %lu->%lu", s0.stacks_live, s1.stacks_live,
              frames0, pmm_free_frames());
    kprintf("[TEST] threads ok: interleaved '%s', exit codes joined, stacks freed\n", trace);
}

/* ---- Day 11: preemption and sleeping ---- */
static volatile bool spin_stop;
static volatile uint64_t spin_count[3];

static void spinner(void *arg) {
    unsigned i = (unsigned)(uintptr_t)arg;
    while (!spin_stop) spin_count[i]++;      /* never yields: must be preempted */
}

static uint64_t wake_at[2];
static unsigned wake_order[2], wake_n;

static void sleeper(void *arg) {
    unsigned i = (unsigned)(uintptr_t)arg;
    uint64_t t0 = timer_ms();
    thread_sleep_ms(i == 0 ? 100 : 50);
    wake_at[i] = timer_ms() - t0;
    wake_order[wake_n++] = i;
}

static void test_preempt(void) {
    struct sched_stats s0, s1;
    sched_get_stats(&s0);
    spin_stop = false;
    struct thread *t[3];
    for (unsigned i = 0; i < 3; i++) {
        spin_count[i] = 0;
        t[i] = thread_create("spinner", spinner, (void *)(uintptr_t)i);
        kassert(t[i]);
    }
    thread_sleep_ms(300);
    spin_stop = true;
    uint64_t ticks[3];
    for (unsigned i = 0; i < 3; i++) ticks[i] = t[i]->ticks_run;
    for (unsigned i = 0; i < 3; i++) thread_join(t[i]);
    sched_get_stats(&s1);
    uint64_t pre = s1.preemptions - s0.preemptions;
    for (unsigned i = 0; i < 3; i++)
        if (!spin_count[i] || ticks[i] < 5)
            panic("preempt: spinner %u count %lu ticks %lu", i, spin_count[i], ticks[i]);
    if (pre < 10) panic("preempt: only %lu preemptions", pre);
    kprintf("[TEST] sched preempt ok: 3 busy threads ran %lu/%lu/%lu ticks, %lu preemptions\n",
            ticks[0], ticks[1], ticks[2], pre);

    wake_n = 0;
    struct thread *a = thread_create("sleep-100", sleeper, (void *)0);
    struct thread *b = thread_create("sleep-50", sleeper, (void *)1);
    kassert(a && b);
    thread_join(a);
    thread_join(b);
    if (wake_n != 2 || wake_order[0] != 1 || wake_order[1] != 0 ||
        wake_at[0] < 100 || wake_at[0] > 130 || wake_at[1] < 50 || wake_at[1] > 80)
        panic("sleep: order %u,%u took %lu/%lu ms", wake_order[0], wake_order[1],
              wake_at[0], wake_at[1]);
    kprintf("[TEST] sched sleep ok: 50 ms -> %lu ms, 100 ms -> %lu ms, woke in order\n",
            wake_at[1], wake_at[0]);
}

/* ---- Day 12: mutex, semaphores (producer/consumer), spinlock ---- */
static struct mutex mtx;
static volatile uint64_t shared_counter;

static void mutex_worker(void *arg) {
    (void)arg;
    for (int i = 0; i < 100; i++) {
        mutex_lock(&mtx);
        uint64_t v = shared_counter;
        if (i % 10 == 0) {
            /* hold the lock across a timer tick so preemption hits the critical section */
            uint64_t t = timer_ticks();
            while (timer_ticks() == t) __asm__ volatile("pause");
        }
        shared_counter = v + 1;
        mutex_unlock(&mtx);
    }
}

#define PC_SLOTS 16
#define PC_ITEMS 1000
static uint64_t pc_buf[PC_SLOTS];
static unsigned pc_in, pc_out;
static struct semaphore pc_empty, pc_full;
static struct mutex pc_lock;
static uint64_t pc_sum[2], pc_got[2];

static void producer(void *arg) {
    uint64_t base = (uintptr_t)arg * 100000;
    for (uint64_t i = 1; i <= PC_ITEMS; i++) {
        sem_wait(&pc_empty);
        mutex_lock(&pc_lock);
        pc_buf[pc_in] = base + i;
        pc_in = (pc_in + 1) % PC_SLOTS;
        mutex_unlock(&pc_lock);
        sem_post(&pc_full);
        if (i % 97 == 0) thread_yield();
    }
}

static void consumer(void *arg) {
    unsigned id = (unsigned)(uintptr_t)arg;
    for (int i = 0; i < PC_ITEMS; i++) {
        sem_wait(&pc_full);
        mutex_lock(&pc_lock);
        uint64_t v = pc_buf[pc_out];
        pc_out = (pc_out + 1) % PC_SLOTS;
        mutex_unlock(&pc_lock);
        sem_post(&pc_empty);
        pc_sum[id] += v;
        pc_got[id]++;
    }
}

void test_sync(void) {
    mutex_init(&mtx);
    shared_counter = 0;
    struct thread *w[3];
    for (int i = 0; i < 3; i++) { w[i] = thread_create("mutex-worker", mutex_worker, NULL); kassert(w[i]); }
    for (int i = 0; i < 3; i++) thread_join(w[i]);
    if (shared_counter != 300 || mtx.contended == 0)
        panic("mutex: counter %lu (want 300), contended %lu", shared_counter, mtx.contended);
    kprintf("[TEST] sync: mutex counter 300, %lu contended locks\n", mtx.contended);

    sem_init(&pc_empty, PC_SLOTS);
    sem_init(&pc_full, 0);
    mutex_init(&pc_lock);
    pc_in = pc_out = 0;
    pc_sum[0] = pc_sum[1] = pc_got[0] = pc_got[1] = 0;
    struct thread *t[4];
    t[0] = thread_create("consumer-0", consumer, (void *)0);
    t[1] = thread_create("producer-1", producer, (void *)1);
    t[2] = thread_create("consumer-1", consumer, (void *)1);
    t[3] = thread_create("producer-2", producer, (void *)2);
    for (int i = 0; i < 4; i++) kassert(t[i]);
    for (int i = 0; i < 4; i++) thread_join(t[i]);
    /* sum over producers p of sum_{i=1..N} (p*100000 + i) */
    uint64_t want = 3ull * 100000 * PC_ITEMS + 2ull * PC_ITEMS * (PC_ITEMS + 1) / 2;
    if (pc_sum[0] + pc_sum[1] != want || pc_got[0] != PC_ITEMS || pc_got[1] != PC_ITEMS ||
        pc_empty.count != PC_SLOTS || pc_full.count != 0)
        panic("prodcons: sum %lu want %lu got %lu/%lu", pc_sum[0] + pc_sum[1], want,
              pc_got[0], pc_got[1]);

    struct spinlock sl = SPINLOCK_INIT("test");
    uint64_t f = spin_lock_irqsave(&sl);
    uint64_t rf;
    __asm__ volatile("pushfq; popq %0" : "=r"(rf));
    bool irqs_off = !(rf & (1u << 9));
    spin_unlock_irqrestore(&sl, f);
    __asm__ volatile("pushfq; popq %0" : "=r"(rf));
    if (!irqs_off || !(rf & (1u << 9)) || sl.locked) panic("spinlock: irq state wrong");
    kprintf("[TEST] sync ok: producer/consumer 2x%u items, sum %lu, spinlock irq-safe\n",
            PC_ITEMS, want);
}

void test_sched(void) {
    test_coop();
    sched_set_preempt(true);
    test_preempt();
    test_sync();
    sched_dump();
}
