/* Interrupt-controller selection: legacy 8259 PIC or local + I/O APIC. */
#include "irq.h"
#include "pic.h"
#include "acpi.h"
#include "kprintf.h"

static enum irq_mode mode = IRQ_MODE_PIC;

void irq_init(const struct boot_info *bi, bool allow_apic) {
    pic_init();                         /* remap + mask everything even in APIC mode, so
                                           a stray 8259 interrupt can't alias exceptions */
    bool have_acpi = acpi_init(bi);
    if (allow_apic && have_acpi && apic_init()) {
        pic_disable();
        mode = IRQ_MODE_APIC;
    }
}

enum irq_mode irq_mode(void) { return mode; }

void irq_unmask(unsigned isa_irq) {
    if (mode == IRQ_MODE_APIC) ioapic_set(isa_irq, IRQ_VEC_FIRST + isa_irq, false);
    else pic_unmask(isa_irq);
}

void irq_mask(unsigned isa_irq) {
    if (mode == IRQ_MODE_APIC) ioapic_set(isa_irq, IRQ_VEC_FIRST + isa_irq, true);
    else pic_mask(isa_irq);
}

bool irq_ack(uint64_t vector) {
    if (mode == IRQ_MODE_APIC) { lapic_eoi(); return true; }
    if (vector >= IRQ_VEC_FIRST + 16) return true;   /* nothing above 47 in PIC mode */
    unsigned irq = vector - IRQ_VEC_FIRST;
    if (pic_is_spurious(irq)) return false;
    pic_eoi(irq);
    return true;
}
