#ifndef TERN_TTY_H
#define TERN_TTY_H
/* Console TTY (Day 22): reading /dev/console.
 *
 * Input comes from the PS/2 keyboard and the COM1 serial port (so `make run-headless`
 * can be typed into). Canonical line discipline: characters are echoed and collected
 * into a line that becomes readable when Enter is pressed.
 *   Backspace / DEL  erase one character      Ctrl-U  erase the line
 *   Ctrl-C           kill the foreground process (see tty_set_fg), or discard the
 *                    line ("^C")             Ctrl-D  end of file at the start of a
 *                                                     line, otherwise submit the line
 * A read returns at most one line (including its '\n'); leftover bytes of a long line
 * are returned by the next read. Reads block (the thread sleeps) until input arrives. */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define TTY_LINE_MAX 254

void tty_init(void);
/* Called from the keyboard / serial IRQ handlers when new input is buffered. */
void tty_input_ready(void);
/* Blocks; returns bytes, 0 at end of file, -EINTR if the reader's process is killed. */
long tty_read(void *buf, size_t len);
/* Foreground process for Ctrl-C (0 = none). Returns the previous one. */
int tty_set_fg(int pid);
/* Called by the input drivers (IRQ context) when Ctrl-C arrives: kills the foreground
 * process and returns true (the character is consumed), or returns false when there is
 * no live foreground process (the character is queued for the line discipline). */
bool tty_intr(void);

struct tty_stats { uint64_t lines, chars, erased, eofs, interrupts, fg_kills; };
void tty_get_stats(struct tty_stats *s);

#endif
