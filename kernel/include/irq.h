#ifndef TERN_IRQ_H
#define TERN_IRQ_H
/* Hardware interrupt routing. Two modes:
 *   PIC:  legacy 8259 pair, ISA IRQ n -> vector 32+n, PIT drives the tick.
 *   APIC: 8259s fully masked; ISA IRQs go through the I/O APIC (honouring MADT source
 *         overrides) to the same vectors 32+n; the local APIC timer drives the tick on
 *         vector 48. Chosen at boot if the CPU has an APIC and ACPI has a MADT, unless
 *         the kernel command line says "noapic".
 * Vectors 32..63 are "hardware IRQ" vectors: they get an EOI before the handler runs
 * and sched_irq_exit() afterwards. */
#include <stdint.h>
#include <stdbool.h>
#include "multiboot2.h"

#define IRQ_VEC_FIRST       32
#define IRQ_VEC_LAPIC_TIMER 48
#define IRQ_VEC_LAST        63
#define LAPIC_SPURIOUS_VEC  0xFF

enum irq_mode { IRQ_MODE_PIC, IRQ_MODE_APIC };

void irq_init(const struct boot_info *bi, bool allow_apic);
enum irq_mode irq_mode(void);
void irq_unmask(unsigned isa_irq);
void irq_mask(unsigned isa_irq);
/* Called by isr_dispatch for vectors 32..63: false if spurious (handler must not run). */
bool irq_ack(uint64_t vector);

/* Local / I/O APIC (apic.c) */
bool apic_init(void);                       /* uses acpi_madt(); false if unusable */
void lapic_eoi(void);
uint32_t lapic_id(void);
uint32_t lapic_version(void);
unsigned ioapic_pins(void);
void ioapic_set(unsigned isa_irq, uint8_t vector, bool masked);
/* Measure the LAPIC timer against PIT channel 2, then run it periodically at hz. */
uint32_t lapic_timer_start(unsigned hz);    /* returns counts per tick (0 = failed) */
uint64_t lapic_spurious_count(void);

#endif
