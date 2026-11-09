/* Interrupt Descriptor Table, exception reporting and handler dispatch. */
#include "idt.h"
#include "gdt.h"
#include "kernel.h"
#include "kprintf.h"
#include "extable.h"
#include "irq.h"
#include "sched.h"
#include "ksyms.h"
#include "proc.h"

struct idt_gate {
    uint16_t off_lo, sel;
    uint8_t ist, type_attr;
    uint16_t off_mid;
    uint32_t off_hi, zero;
} __attribute__((packed));

struct idt_ptr { uint16_t limit; uint64_t base; } __attribute__((packed));

static struct idt_gate idt[256] __attribute__((aligned(16)));
static isr_handler_t handlers[256];

extern char isr_stubs[], isr_stubs_end[];
extern const struct extable_entry __extable_start[], __extable_end[];

static const char *const exc_names[32] = {
    "divide error", "debug", "NMI", "breakpoint", "overflow", "bound range",
    "invalid opcode", "device not available", "double fault", "coprocessor overrun",
    "invalid TSS", "segment not present", "stack fault", "general protection",
    "page fault", "reserved", "x87 FP error", "alignment check", "machine check",
    "SIMD FP error", "virtualization", "control protection", "reserved", "reserved",
    "reserved", "reserved", "reserved", "reserved", "hypervisor injection",
    "VMM communication", "security", "reserved",
};

const char *exception_name(uint64_t v) {
    if (v < 32) return exc_names[v];
    if (v >= IRQ_VEC_FIRST && v <= IRQ_VEC_LAST) return "IRQ";
    return "interrupt";
}

static void set_gate(uint8_t v, uint64_t addr, uint8_t ist, uint8_t dpl) {
    idt[v].off_lo = addr & 0xFFFF;
    idt[v].sel = GDT_KCODE;
    idt[v].ist = ist;
    idt[v].type_attr = 0x80 | (dpl << 5) | 0xE;    /* present, 64-bit interrupt gate */
    idt[v].off_mid = (addr >> 16) & 0xFFFF;
    idt[v].off_hi = addr >> 32;
    idt[v].zero = 0;
}

void idt_init(void) {
    kassert(isr_stubs_end - isr_stubs == 256 * 16);
    for (int v = 0; v < 256; v++)
        set_gate(v, (uint64_t)(isr_stubs + 16 * v), v == 8 ? 1 : 0, 0);
    struct idt_ptr p = { sizeof idt - 1, (uint64_t)idt };
    __asm__ volatile("lidt %0" : : "m"(p));
}

isr_handler_t isr_set_handler(uint8_t v, isr_handler_t h) {
    isr_handler_t old = handlers[v];
    handlers[v] = h;
    return old;
}

bool extable_fixup(struct regs *r) {
    for (const struct extable_entry *e = __extable_start; e < __extable_end; e++)
        if (e->insn == r->rip) { r->rip = e->fixup; return true; }
    return false;
}

void regs_dump(const struct regs *r) {
    uint64_t cr2, cr3;
    __asm__ volatile("mov %%cr2, %0; mov %%cr3, %1" : "=r"(cr2), "=r"(cr3));
    kprintf("vector %lu (%s) error %lx\n", r->vector, exception_name(r->vector), r->error);
    kprintf("rip %016lx cs %04lx rflags %016lx\n", r->rip, r->cs, r->rflags);
    kprintf("rsp %016lx ss %04lx cr2 %016lx cr3 %016lx\n", r->rsp, r->ss, cr2, cr3);
    kprintf("rax %016lx rbx %016lx rcx %016lx rdx %016lx\n", r->rax, r->rbx, r->rcx, r->rdx);
    kprintf("rsi %016lx rdi %016lx rbp %016lx r8  %016lx\n", r->rsi, r->rdi, r->rbp, r->r8);
    kprintf("r9  %016lx r10 %016lx r11 %016lx r12 %016lx\n", r->r9, r->r10, r->r11, r->r12);
    kprintf("r13 %016lx r14 %016lx r15 %016lx\n", r->r13, r->r14, r->r15);
    if (r->vector == 14)
        kprintf("page fault: %s %s %s%s\n", r->error & 1 ? "protection" : "not-present",
                r->error & 2 ? "write" : "read", r->error & 4 ? "user" : "kernel",
                r->error & 16 ? " ifetch" : "");
}

void isr_dispatch(struct regs *r) {
    uint64_t v = r->vector;
    if (v >= IRQ_VEC_FIRST && v <= IRQ_VEC_LAST) {
        if ((r->cs & 3) && proc_current()) proc_current()->user_irqs++;
        if (!irq_ack(v)) return;   /* EOI first: a handler may switch tasks and not return soon */
        if (handlers[v]) handlers[v](r);
        sched_irq_exit();  /* may switch threads; we come back here when rescheduled */
        if ((r->cs & 3) == 3) proc_check_kill();   /* pending kill of a ring-3 spinner */
        return;
    }
    if (handlers[v]) { handlers[v](r); return; }
    if (v < 32) {
        if (v == 14 && !(r->error & 1)) {           /* not present: lazy heap/stack page? */
            uint64_t cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
            if (cr2 < 0x0000800000000000ull && proc_demand_fault(cr2)) return;
        }
        if (v == 14 && (r->error & 3) == 3) {       /* write to a present page: COW? */
            uint64_t cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
            if (cr2 < 0x0000800000000000ull && proc_cow_fault(cr2)) return;
        }
        if ((r->cs & 3) == 3) proc_user_fault(r);    /* kills the process, not the kernel */
        if ((v == 13 || v == 14) && (r->cs & 3) == 0 && extable_fixup(r)) return;
        kprintf("\n*** CPU EXCEPTION\n");
        if (v == 8) {
            uint64_t cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
            sched_check_stack_overflow(cr2);
        }
        regs_dump(r);
        backtrace_print(r->rip, r->rbp);
        g_panic_backtrace_done = true;
        panic("unhandled exception %lu (%s) at %lx", v, exception_name(v), r->rip);
    }
    kprintf("unexpected interrupt vector %lu\n", v);
}
