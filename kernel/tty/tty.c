/* Console TTY: blocking input + canonical line discipline (see tty.h). */
#include "tty.h"
#include "keyboard.h"
#include "serial.h"
#include "console.h"
#include "sched.h"
#include "sync.h"
#include "idt.h"
#include "string.h"
#include "proc.h"
#include "syscall_nr.h"

static struct waitq input_wq;          /* readers waiting for a character */
static struct mutex read_lock;         /* one reader edits the line at a time */
static char line[TTY_LINE_MAX + 2];    /* line being edited (+ '\n') */
static size_t line_len;
static char ready[TTY_LINE_MAX + 2];   /* submitted line not yet fully read */
static size_t ready_len, ready_pos;
static struct tty_stats stats;
static bool inited;
static int fg_pid;

void tty_init(void) {
    waitq_init(&input_wq);
    mutex_init(&read_lock);
    inited = true;
}

void tty_input_ready(void) {
    if (inited) waitq_wake_all(&input_wq);
}

int tty_set_fg(int pid) {
    uint64_t f = irq_save();
    int old = fg_pid;
    fg_pid = pid > 0 ? pid : 0;
    irq_restore(f);
    return old;
}

bool tty_intr(void) {
    if (!inited || !fg_pid) return false;
    if (proc_kill(fg_pid, TERN_EXIT_INTR) != 0) { fg_pid = 0; return false; }
    stats.fg_kills++;
    console_write("^C\n");
    return true;
}

/* Next input character from any source; sleeps while there is none. -EINTR if our
 * process gets killed meanwhile. */
static int getc_blocking(void) {
    for (;;) {
        uint64_t f = irq_save();
        if (proc_kill_pending()) { irq_restore(f); return -EINTR; }
        int c = kbd_getc();
        if (c < 0) c = serial_getc();
        if (c >= 0) { irq_restore(f); return c; }
        waitq_sleep(&input_wq);       /* the IRQ handler wakes us */
        irq_restore(f);
    }
}

static void echo(const char *s) { console_write(s); }

static void submit(void) {
    memcpy(ready, line, line_len);
    ready_len = line_len;
    ready_pos = 0;
    line_len = 0;
    stats.lines++;
}

long tty_read(void *buf, size_t len) {
    if (len == 0) return 0;
    mutex_lock(&read_lock);
    long rc = -1;
    while (ready_pos == ready_len) {
        int c = getc_blocking();
        if (c == -EINTR) { rc = -EINTR; break; }
        if (c == '\r') c = '\n';                    /* serial terminals send CR */
        if (c == 0x7F) c = '\b';
        if (c == '\n') {
            line[line_len++] = '\n';
            echo("\n");
            submit();
        } else if (c == '\b') {
            if (line_len) { line_len--; stats.erased++; echo("\b \b"); }
        } else if (c == 0x15) {                     /* ^U */
            while (line_len) { line_len--; stats.erased++; echo("\b \b"); }
        } else if (c == 0x03) {                     /* ^C */
            line_len = 0;
            stats.interrupts++;
            echo("^C\n");
        } else if (c == 0x04) {                     /* ^D */
            if (line_len == 0) { stats.eofs++; rc = 0; break; }
            submit();
        } else if ((c >= 0x20 && c < 0x7F) || c == '\t') {
            if (line_len < TTY_LINE_MAX) {
                line[line_len++] = (char)c;
                stats.chars++;
                char e[2] = { (char)c, 0 };
                echo(e);
            }
        }                                           /* other control chars: ignored */
    }
    if (rc == -1) {                                 /* a line is ready */
        size_t n = ready_len - ready_pos;
        if (n > len) n = len;
        memcpy(buf, ready + ready_pos, n);
        ready_pos += n;
        rc = (long)n;
    }
    mutex_unlock(&read_lock);
    return rc;
}

void tty_get_stats(struct tty_stats *s) { *s = stats; }
