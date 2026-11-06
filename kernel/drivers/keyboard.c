/* PS/2 keyboard on the 8042 controller. The controller translates to scancode set 1,
 * which we map to ASCII (US layout) with Shift / Caps Lock / Ctrl handling. */
#include "keyboard.h"
#include "io.h"
#include "idt.h"
#include "irq.h"
#include "timer.h"
#include "tty.h"

#define KBD_DATA 0x60
#define KBD_STATUS 0x64     /* read */
#define KBD_CMD 0x64        /* write */

#define RING 256
static volatile char ring[RING];
static volatile unsigned head, tail;      /* head: next write, tail: next read */
static uint64_t dropped;
static bool shift_l, shift_r, caps, ctrl, extended;

/* Set 1 make codes 0x00..0x39 */
static const char normal[0x3A] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' ',
};
static const char shifted[0x3A] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' ',
};

static void push(char c) {
    unsigned next = (head + 1) % RING;
    if (next == tail) { dropped++; return; }
    ring[head] = c;
    head = next;
}

static void handle_scancode(uint8_t sc) {
    if (sc == 0xE0) { extended = true; return; }
    bool release = sc & 0x80;
    uint8_t code = sc & 0x7F;
    bool ext = extended;
    extended = false;
    if (ext) {                          /* right ctrl; arrows etc. ignored for now */
        if (code == 0x1D) ctrl = !release;
        return;
    }
    switch (code) {
    case 0x2A: shift_l = !release; return;
    case 0x36: shift_r = !release; return;
    case 0x1D: ctrl = !release; return;
    case 0x3A: if (!release) caps = !caps; return;
    }
    if (release || code >= sizeof normal) return;
    bool shift = shift_l || shift_r;
    char c = shift ? shifted[code] : normal[code];
    if (!c) return;
    if (caps && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) c ^= 0x20;
    if (ctrl && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) c &= 0x1F;
    push(c);
}

static void kbd_irq(struct regs *r) {
    (void)r;
    while (inb(KBD_STATUS) & 1) handle_scancode(inb(KBD_DATA));
    tty_input_ready();
}

static bool wait_write(void) {
    for (int i = 0; i < 100000; i++) if (!(inb(KBD_STATUS) & 2)) return true;
    return false;
}
static bool wait_read(void) {
    for (int i = 0; i < 100000; i++) if (inb(KBD_STATUS) & 1) return true;
    return false;
}
static void ctl_cmd(uint8_t c) { wait_write(); outb(KBD_CMD, c); }

bool kbd_init(void) {
    if (inb(KBD_STATUS) == 0xFF) return false;          /* no controller */
    ctl_cmd(0xAD);                                      /* disable port 1 */
    ctl_cmd(0xA7);                                      /* disable port 2 */
    for (int i = 0; i < 64 && (inb(KBD_STATUS) & 1); i++) inb(KBD_DATA);   /* flush */
    ctl_cmd(0x20);                                      /* read config byte */
    if (!wait_read()) return false;
    uint8_t cfg = inb(KBD_DATA);
    cfg |= 0x01 | 0x40;                                 /* port-1 IRQ, set-1 translation */
    cfg &= ~0x02;                                       /* no port-2 IRQ (mouse later) */
    ctl_cmd(0x60);
    wait_write(); outb(KBD_DATA, cfg);
    ctl_cmd(0xAE);                                      /* enable port 1 */
    head = tail = 0;
    isr_set_handler(IRQ_BASE + 1, kbd_irq);
    irq_unmask(1);
    return true;
}

int kbd_getc(void) {
    uint64_t f = irq_save();
    int c = -1;
    if (tail != head) { c = (unsigned char)ring[tail]; tail = (tail + 1) % RING; }
    irq_restore(f);
    return c;
}

int kbd_getc_timeout(uint64_t ms) {
    uint64_t end = timer_ms() + ms;
    for (;;) {
        int c = kbd_getc();
        if (c >= 0 || timer_ms() >= end) return c;
        __asm__ volatile("hlt");
    }
}

uint64_t kbd_dropped(void) { return dropped; }
