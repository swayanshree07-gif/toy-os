/* Global Descriptor Table + 64-bit Task State Segment. */
#include "gdt.h"
#include "string.h"

struct gdt_ptr { uint16_t limit; uint64_t base; } __attribute__((packed));

static uint64_t gdt[7] __attribute__((aligned(16)));
struct tss g_tss __attribute__((aligned(16)));
static uint8_t df_stack[8192] __attribute__((aligned(16)));   /* IST1: double fault */

extern void gdt_load(const struct gdt_ptr *p, uint16_t code, uint16_t data);

static uint64_t seg(uint8_t access, uint8_t flags) {
    /* base 0, limit 0xFFFFF; in long mode base/limit are ignored for code/data */
    return 0xFFFFull | (0xFull << 48) | ((uint64_t)access << 40) | ((uint64_t)flags << 52);
}

void gdt_init(void) {
    gdt[0] = 0;
    gdt[1] = seg(0x9A, 0xA);    /* kernel code: P, DPL0, code, R; flags G|L */
    gdt[2] = seg(0x92, 0xC);    /* kernel data: P, DPL0, data, W; flags G|DB */
    gdt[3] = seg(0xF2, 0xC);    /* user data: DPL3 */
    gdt[4] = seg(0xFA, 0xA);    /* user code: DPL3, L */

    memset(&g_tss, 0, sizeof g_tss);
    g_tss.iomap_base = sizeof g_tss;     /* no I/O permission bitmap */
    g_tss.ist[0] = (uint64_t)(df_stack + sizeof df_stack);

    uint64_t base = (uint64_t)&g_tss, limit = sizeof g_tss - 1;
    gdt[5] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ull << 40) |
             (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    gdt[6] = base >> 32;

    struct gdt_ptr p = { sizeof gdt - 1, (uint64_t)gdt };
    gdt_load(&p, GDT_KCODE, GDT_KDATA);
    __asm__ volatile("ltr %w0" : : "r"((uint16_t)GDT_TSS));
}

/* Kernel stack for ring-3 -> ring-0 transitions: the CPU takes it from TSS.rsp0 for
 * interrupts; syscall_entry (usermode.S) loads g_syscall_kstack itself. */
uint64_t g_syscall_kstack;

void tss_set_kernel_stack(uint64_t rsp0) {
    g_tss.rsp[0] = rsp0;
    g_syscall_kstack = rsp0;
}
