/* 8259A programmable interrupt controller pair (master 0x20, slave 0xA0). */
#include "pic.h"
#include "io.h"
#include "idt.h"

#define PIC1_CMD 0x20
#define PIC1_DATA 0x21
#define PIC2_CMD 0xA0
#define PIC2_DATA 0xA1

static uint64_t spurious;

void pic_init(void) {
    outb(PIC1_CMD, 0x11); io_wait();          /* ICW1: init, expect ICW4 */
    outb(PIC2_CMD, 0x11); io_wait();
    outb(PIC1_DATA, IRQ_BASE); io_wait();     /* ICW2: vector offsets */
    outb(PIC2_DATA, IRQ_BASE + 8); io_wait();
    outb(PIC1_DATA, 4); io_wait();            /* ICW3: slave on IRQ2 */
    outb(PIC2_DATA, 2); io_wait();
    outb(PIC1_DATA, 1); io_wait();            /* ICW4: 8086 mode */
    outb(PIC2_DATA, 1); io_wait();
    outb(PIC1_DATA, 0xFF & ~(1 << 2));        /* all masked except the cascade line */
    outb(PIC2_DATA, 0xFF);
}

void pic_unmask(unsigned irq) {
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    outb(port, inb(port) & ~(1 << (irq & 7)));
}

void pic_mask(unsigned irq) {
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    outb(port, inb(port) | (1 << (irq & 7)));
}

void pic_eoi(unsigned irq) {
    if (irq >= 8) outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}

static uint8_t pic_isr(uint16_t cmd) {
    outb(cmd, 0x0B);                          /* OCW3: read in-service register */
    return inb(cmd);
}

bool pic_is_spurious(unsigned irq) {
    if (irq == 7 && !(pic_isr(PIC1_CMD) & 0x80)) { spurious++; return true; }
    if (irq == 15 && !(pic_isr(PIC2_CMD) & 0x80)) {
        spurious++;
        outb(PIC1_CMD, 0x20);                 /* master did see the cascade IRQ */
        return true;
    }
    return false;
}

uint64_t pic_spurious_count(void) { return spurious; }
