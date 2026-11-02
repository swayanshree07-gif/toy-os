/* COM1 16550 UART driver (polling). */
#include "serial.h"
#include "io.h"

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
