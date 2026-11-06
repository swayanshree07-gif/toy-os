#ifndef TERN_SERIAL_H
#define TERN_SERIAL_H
#include <stdbool.h>

bool serial_init(void);          /* COM1, 115200 8N1; returns false if loopback test fails */
void serial_putc(char c);        /* translates '\n' to "\r\n" */
void serial_write(const char *s);
void serial_enable_rx_irq(void); /* after irq_init: received bytes are buffered */
int serial_getc(void);           /* next received byte or -1 */

#endif
