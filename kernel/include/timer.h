#ifndef TERN_TIMER_H
#define TERN_TIMER_H
#include <stdint.h>

#define TIMER_HZ 100

void pit_init(unsigned hz);           /* PIT channel 0, IRQ 0 */
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
