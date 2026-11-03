#ifndef TERN_IDT_H
#define TERN_IDT_H
#include <stdint.h>
#include <stdbool.h>

/* Register frame built by the ISR stubs (isr.S); lowest address first. */
struct regs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags, rsp, ss;   /* pushed by the CPU */
};

typedef void (*isr_handler_t)(struct regs *r);

#define IRQ_BASE 32                       /* PIC IRQ 0..15 -> vectors 32..47 */

void idt_init(void);
/* Install a handler for a vector. For exception vectors the handler replaces the
 * default "dump and panic"; it may change r->rip etc. to recover. Returns the old one. */
isr_handler_t isr_set_handler(uint8_t vector, isr_handler_t h);
void regs_dump(const struct regs *r);
const char *exception_name(uint64_t vector);

/* Exception fixup table (see extable.h): if a kernel fault hits a listed instruction,
 * execution resumes at its fixup address instead of panicking. */
bool extable_fixup(struct regs *r);

static inline void irq_enable(void)  { __asm__ volatile("sti" ::: "memory"); }
static inline void irq_disable(void) { __asm__ volatile("cli" ::: "memory"); }
static inline uint64_t irq_save(void) {
    uint64_t f;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) : : "memory");
    return f;
}
static inline void irq_restore(uint64_t f) {
    if (f & (1u << 9)) irq_enable();
}

#endif
