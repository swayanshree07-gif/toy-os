/* Spinlocks, mutexes and semaphores on top of the scheduler's wait queues.
 * Single CPU: "interrupts off" is what really protects the wait-queue state; the
 * spinlock's atomic flag additionally catches recursive locking. */
#include "sync.h"
#include "idt.h"
#include "kernel.h"

uint64_t spin_lock_irqsave(struct spinlock *l) {
    uint64_t f = irq_save();
    while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE)) {
        /* On one CPU with IRQs off nobody else can release it: this is a deadlock. */
        panic("spinlock '%s' already held (recursive lock?)", l->name ? l->name : "?");
    }
    return f;
}

void spin_unlock_irqrestore(struct spinlock *l, uint64_t flags) {
    kassert(l->locked);
    __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
    irq_restore(flags);
}

void mutex_init(struct mutex *m) {
    m->owner = NULL;
    m->contended = 0;
    waitq_init(&m->waiters);
}

void mutex_lock(struct mutex *m) {
    uint64_t f = irq_save();
    kassert(m->owner != thread_current());
    if (m->owner) m->contended++;
    while (m->owner) waitq_sleep(&m->waiters);
    m->owner = thread_current();
    irq_restore(f);
}

bool mutex_trylock(struct mutex *m) {
    uint64_t f = irq_save();
    bool ok = m->owner == NULL;
    if (ok) m->owner = thread_current();
    irq_restore(f);
    return ok;
}

void mutex_unlock(struct mutex *m) {
    uint64_t f = irq_save();
    kassert(m->owner == thread_current());
    m->owner = NULL;
    waitq_wake_one(&m->waiters);
    irq_restore(f);
}

void sem_init(struct semaphore *s, int64_t count) {
    s->count = count;
    waitq_init(&s->waiters);
}

void sem_wait(struct semaphore *s) {
    uint64_t f = irq_save();
    while (s->count <= 0) waitq_sleep(&s->waiters);
    s->count--;
    irq_restore(f);
}

void sem_post(struct semaphore *s) {
    uint64_t f = irq_save();
    s->count++;
    waitq_wake_one(&s->waiters);
    irq_restore(f);
}
