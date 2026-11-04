/* Local APIC (MMIO) + I/O APIC driver, single CPU. */
#include "irq.h"
#include "acpi.h"
#include "vmm.h"
#include "timer.h"
#include "kprintf.h"
#include "idt.h"

#define LAPIC_ID      0x020
#define LAPIC_VER     0x030
#define LAPIC_TPR     0x080
#define LAPIC_EOI     0x0B0
#define LAPIC_SVR     0x0F0
#define LAPIC_ESR     0x280
#define LAPIC_LVT_TMR 0x320
#define LAPIC_LINT0   0x350
#define LAPIC_LINT1   0x360
#define LAPIC_LVT_ERR 0x370
#define LAPIC_TMR_INIT 0x380
#define LAPIC_TMR_CUR 0x390
#define LAPIC_TMR_DIV 0x3E0
#define LVT_MASKED    (1u << 16)
#define LVT_PERIODIC  (1u << 17)

#define IA32_APIC_BASE 0x1B

static volatile uint32_t *lapic;
static volatile uint32_t *ioapic;
static uint32_t ioapic_gsi_base;
static unsigned ioapic_npins;
static uint64_t spurious;

static inline uint32_t lr(uint32_t reg) { return lapic[reg / 4]; }
static inline void lw(uint32_t reg, uint32_t v) { lapic[reg / 4] = v; }

static uint32_t ior(uint32_t reg) { ioapic[0] = reg; return ioapic[4]; }
static void iow(uint32_t reg, uint32_t v) { ioapic[0] = reg; ioapic[4] = v; }

static uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static void wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

static void spurious_handler(struct regs *r) { (void)r; spurious++; }   /* no EOI */
static void lapic_error_handler(struct regs *r) {
    (void)r;
    lw(LAPIC_ESR, 0);
    kprintf("lapic: error, esr=%x\n", lr(LAPIC_ESR));
}

void lapic_eoi(void) { lw(LAPIC_EOI, 0); }
uint32_t lapic_id(void) { return lr(LAPIC_ID) >> 24; }
uint32_t lapic_version(void) { return lr(LAPIC_VER) & 0xFF; }
unsigned ioapic_pins(void) { return ioapic_npins; }
uint64_t lapic_spurious_count(void) { return spurious; }

bool apic_init(void) {
    const struct madt_info *m = acpi_madt();
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
    if (!(d & (1u << 9))) { kprintf("apic: cpu has no local APIC\n"); return false; }
    if (!m->valid || m->ioapic_count == 0) { kprintf("apic: no usable MADT\n"); return false; }

    uint64_t base = rdmsr(IA32_APIC_BASE);
    uint64_t phys = base & 0x000FFFFFFFFFF000ull;
    if (phys != m->lapic_phys)
        kprintf("apic: MSR base %lx differs from MADT %lx, using MSR\n", phys, m->lapic_phys);
    wrmsr(IA32_APIC_BASE, base | (1u << 11));            /* global enable */
    lapic = vmm_map_mmio(phys, 0x1000);
    ioapic = vmm_map_mmio(m->ioapics[0].phys, 0x20);
    if (!lapic || !ioapic) { kprintf("apic: mmio map failed\n"); return false; }
    ioapic_gsi_base = m->ioapics[0].gsi_base;
    ioapic_npins = ((ior(1) >> 16) & 0xFF) + 1;

    /* Mask every I/O APIC pin until a driver asks for it. */
    for (unsigned i = 0; i < ioapic_npins; i++) {
        iow(0x10 + 2 * i, LVT_MASKED);
        iow(0x11 + 2 * i, 0);
    }

    isr_set_handler(LAPIC_SPURIOUS_VEC, spurious_handler);
    isr_set_handler(0xFE, lapic_error_handler);
    lw(LAPIC_TPR, 0);
    lw(LAPIC_LVT_TMR, LVT_MASKED);
    lw(LAPIC_LINT0, LVT_MASKED);                         /* no ExtINT from the 8259 */
    lw(LAPIC_LINT1, (4u << 8));                          /* NMI */
    lw(LAPIC_LVT_ERR, 0xFE);
    lw(LAPIC_ESR, 0);
    lw(LAPIC_ESR, 0);
    lw(LAPIC_SVR, 0x100 | LAPIC_SPURIOUS_VEC);           /* software enable */
    lapic_eoi();
    return true;
}

void ioapic_set(unsigned isa_irq, uint8_t vector, bool masked) {
    const struct madt_info *m = acpi_madt();
    uint32_t gsi = isa_irq < 16 ? m->isa_gsi[isa_irq] : isa_irq;
    uint16_t fl = isa_irq < 16 ? m->isa_flags[isa_irq] : 0;
    if (gsi < ioapic_gsi_base || gsi >= ioapic_gsi_base + ioapic_npins) return;
    unsigned pin = gsi - ioapic_gsi_base;
    /* MPS INTI flags: polarity bits 0-1 (3 = active low), trigger bits 2-3 (3 = level).
     * 0 means "bus default", which is active high + edge for ISA. */
    uint32_t lo = vector;
    if ((fl & 3) == 3) lo |= 1u << 13;
    if (((fl >> 2) & 3) == 3) lo |= 1u << 15;
    if (masked) lo |= LVT_MASKED;
    iow(0x11 + 2 * pin, lapic_id() << 24);               /* physical destination: us */
    iow(0x10 + 2 * pin, lo);
}

uint32_t lapic_timer_start(unsigned hz) {
    lw(LAPIC_TMR_DIV, 0x3);                              /* divide by 16 */
    lw(LAPIC_LVT_TMR, LVT_MASKED | IRQ_VEC_LAPIC_TIMER);
    lw(LAPIC_TMR_INIT, 0xFFFFFFFFu);
    pit_busy_wait_us(50000);
    uint32_t elapsed = 0xFFFFFFFFu - lr(LAPIC_TMR_CUR);
    lw(LAPIC_TMR_INIT, 0);
    uint64_t per_tick = (uint64_t)elapsed * 20 / hz;     /* elapsed counts per 50 ms */
    if (per_tick == 0 || per_tick > 0xFFFFFFFFu) return 0;
    lw(LAPIC_LVT_TMR, LVT_PERIODIC | IRQ_VEC_LAPIC_TIMER);
    lw(LAPIC_TMR_INIT, (uint32_t)per_tick);
    return (uint32_t)per_tick;
}
