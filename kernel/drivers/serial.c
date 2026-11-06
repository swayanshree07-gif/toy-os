/* COM1 16550 UART driver: polled output, interrupt-driven input (IRQ 4) once
 * serial_enable_rx_irq() has run. */
#include "serial.h"
#include "io.h"
#include "idt.h"
#include "irq.h"
#include "tty.h"

#define COM1 0x3F8

static bool serial_ok;

bool serial_init(void) {
    outb(COM1 + 1, 0x00);   /* disable interrupts */
    outb(COM1 + 3, 0x80);   /* DLAB on */
    outb(COM1 + 0, 0x01);   /* divisor 1 -> 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8N1, DLAB off */
    outb(COM1 + 2, 0xC7);   /* FIFO on, clear, 14-byte threshold */
    outb(COM1 + 4, 0x1E);   /* loopback mode for self-test */
    outb(COM1 + 0, 0xA5);
    if (inb(COM1 + 0) != 0xA5) {
        serial_ok = false;
        return false;
    }
    outb(COM1 + 4, 0x0F);   /* normal mode: DTR, RTS, OUT1, OUT2 */
    serial_ok = true;
    return true;
}

static void serial_raw(char c) {
    for (int spin = 0; spin < 100000 && !(inb(COM1 + 5) & 0x20); spin++)
        ;
    outb(COM1, (uint8_t)c);
}

void serial_putc(char c) {
    if (!serial_ok) return;
    if (c == '\n') serial_raw('\r');
    serial_raw(c);
}

void serial_write(const char *s) {
    while (*s) serial_putc(*s++);
}

/* ---- input ---- */
#define RX_RING 256
static volatile char rx[RX_RING];
static volatile unsigned rx_head, rx_tail;

static void serial_irq(struct regs *r) {
    (void)r;
    while (inb(COM1 + 5) & 1) {
        char c = (char)inb(COM1);
        if (c == 0x03 && tty_intr()) continue;  /* Ctrl-C killed the foreground process */
        unsigned next = (rx_head + 1) % RX_RING;
        if (next != rx_tail) { rx[rx_head] = c; rx_head = next; }
    }
    tty_input_ready();
}

void serial_enable_rx_irq(void) {
    if (!serial_ok) return;
    while (inb(COM1 + 5) & 1) inb(COM1);     /* drop anything received during boot */
    isr_set_handler(IRQ_BASE + 4, serial_irq);
    outb(COM1 + 1, 0x01);                   /* interrupt on received data */
    irq_unmask(4);
}

int serial_getc(void) {
    uint64_t f = irq_save();
    int c = -1;
    if (rx_tail != rx_head) { c = (unsigned char)rx[rx_tail]; rx_tail = (rx_tail + 1) % RX_RING; }
    irq_restore(f);
    return c;
}
