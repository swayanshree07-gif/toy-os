#ifndef TERN_TIMER_H
#define TERN_TIMER_H
#include <stdint.h>

#define TIMER_HZ 100

/* Start the system tick at hz: LAPIC timer in APIC mode, else PIT channel 0 (IRQ 0).
 * Needs irq_init() first. */
void timer_init(unsigned hz);
const char *timer_source(void);       /* "lapic" or "pit" */
uint32_t timer_lapic_counts(void);    /* LAPIC timer counts per tick (0 if not used) */
uint64_t timer_ticks(void);
uint64_t timer_ms(void);
void sleep_ms(uint64_t ms);           /* hlt-waits; needs interrupts enabled */
/* Polled PIT channel 2 busy-wait (no interrupts needed), max ~54 ms. */
void pit_busy_wait_us(uint32_t us);
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

#endif
