#ifndef TERN_KEYBOARD_H
#define TERN_KEYBOARD_H
#include <stdbool.h>
#include <stdint.h>

bool kbd_init(void);          /* 8042 controller + IRQ 1; false if no controller found */
int kbd_getc(void);           /* next ASCII char from the input buffer, or -1 if empty */
int kbd_getc_timeout(uint64_t ms);   /* waits (hlt) up to ms; -1 on timeout */
uint64_t kbd_dropped(void);   /* chars lost because the ring buffer was full */

#endif
