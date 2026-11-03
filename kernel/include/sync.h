#ifndef TERN_SYNC_H
#define TERN_SYNC_H
/* Synchronisation primitives.
 *  spinlock  - busy-wait lock that also disables interrupts (IRQ-safe); short sections
 *  mutex     - sleeping lock with an owner; may not be used from IRQ handlers
 *  semaphore - counting semaphore; sem_post may be called from IRQ handlers */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "sched.h"

struct spinlock { volatile uint32_t locked; const char *name; };
#define SPINLOCK_INIT(n) { 0, n }
uint64_t spin_lock_irqsave(struct spinlock *l);
void spin_unlock_irqrestore(struct spinlock *l, uint64_t flags);

struct mutex { struct thread *owner; struct waitq waiters; uint64_t contended; };
void mutex_init(struct mutex *m);
void mutex_lock(struct mutex *m);
bool mutex_trylock(struct mutex *m);
void mutex_unlock(struct mutex *m);

struct semaphore { int64_t count; struct waitq waiters; };
void sem_init(struct semaphore *s, int64_t count);
void sem_wait(struct semaphore *s);
void sem_post(struct semaphore *s);

void test_sync(void);

#endif
