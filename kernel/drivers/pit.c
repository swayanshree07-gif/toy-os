/* 8253/8254 programmable interval timer: channel 0 drives the system tick. */
#include "timer.h"
#include "io.h"
#include "idt.h"
#include "pic.h"
#include "sched.h"

#define PIT_FREQ 1193182u

static volatile uint64_t ticks;
static unsigned tick_hz = TIMER_HZ;

static void pit_irq(struct regs *r) {
    (void)r;
    ticks++;
    sched_tick(ticks);
}

void pit_init(unsigned hz) {
    tick_hz = hz;
    uint32_t div = (PIT_FREQ + hz / 2) / hz;
    outb(0x43, 0x36);                 /* channel 0, lo/hi byte, mode 3 (square wave) */
    outb(0x40, div & 0xFF);
    outb(0x40, div >> 8);
    isr_set_handler(IRQ_BASE + 0, pit_irq);
    pic_unmask(0);
}

uint64_t timer_ticks(void) { return ticks; }
uint64_t timer_ms(void) { return ticks * 1000 / tick_hz; }

void sleep_ms(uint64_t ms) {
    if (sched_active()) { thread_sleep_ms(ms); return; }
    uint64_t target = ticks + (ms * tick_hz + 999) / 1000;
    while (ticks < target) __asm__ volatile("hlt");
}

void pit_busy_wait_us(uint32_t us) {
    uint32_t count = (uint32_t)((uint64_t)PIT_FREQ * us / 1000000);
    if (count > 0xFFFF) count = 0xFFFF;
    if (count == 0) count = 1;
    uint8_t p61 = inb(0x61);
    outb(0x61, (p61 & ~0x02) | 0x01); /* gate on, speaker off */
    outb(0x43, 0xB0);                 /* channel 2, lo/hi, mode 0 (terminal count) */
    outb(0x42, count & 0xFF);
    outb(0x42, count >> 8);
    while (!(inb(0x61) & 0x20)) { }   /* OUT2 goes high at terminal count */
    outb(0x61, p61);
}
