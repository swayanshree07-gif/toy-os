/* 8253/8254 programmable interval timer and the system tick.
 * The tick comes from PIT channel 0 (IRQ 0) in PIC mode, or from the local APIC timer
 * (calibrated against PIT channel 2) in APIC mode. */
#include "timer.h"
#include "io.h"
#include "idt.h"
#include "irq.h"
#include "kprintf.h"
#include "sched.h"

#define PIT_FREQ 1193182u

static volatile uint64_t ticks;
static unsigned tick_hz = TIMER_HZ;
static const char *source = "none";
static uint32_t lapic_counts;

static void tick_irq(struct regs *r) {
    (void)r;
    ticks++;
    sched_tick(ticks);
}

static void pit_start(unsigned hz) {
    uint32_t div = (PIT_FREQ + hz / 2) / hz;
    outb(0x43, 0x36);                 /* channel 0, lo/hi byte, mode 3 (square wave) */
    outb(0x40, div & 0xFF);
    outb(0x40, div >> 8);
    isr_set_handler(IRQ_VEC_FIRST + 0, tick_irq);
    irq_unmask(0);
}

void timer_init(unsigned hz) {
    tick_hz = hz;
    if (irq_mode() == IRQ_MODE_APIC) {
        isr_set_handler(IRQ_VEC_LAPIC_TIMER, tick_irq);
        lapic_counts = lapic_timer_start(hz);
        if (lapic_counts) { source = "lapic"; return; }
        kprintf("timer: lapic calibration failed, using the PIT\n");
    }
    pit_start(hz);
    source = "pit";
}

const char *timer_source(void) { return source; }
uint32_t timer_lapic_counts(void) { return lapic_counts; }
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
