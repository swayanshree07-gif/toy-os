/* Framebuffer text console + output multiplexer. */
#include "console.h"
#include "fb.h"
#include "serial.h"
#include "string.h"
#include "idt.h"

#define PX_W (CELL_W * CON_SCALE)
#define PX_H (CELL_H * CON_SCALE)

/* Shadow copy of the screen text (Day 11 session): while a user program owns the
 * framebuffer (con_suspend), output only goes here, and con_resume() repaints from it.
 * Screens with more cells than this use only the top-left CON_MAX_COLS x CON_MAX_ROWS. */
#define CON_MAX_COLS 200
#define CON_MAX_ROWS 100
struct cell { char ch; uint32_t fg, bg; };
static struct cell shadow[CON_MAX_ROWS][CON_MAX_COLS];

static bool active, suspended;
static uint32_t cols, rows, cx, cy;
static uint32_t fg, bg;

bool con_init(void) {
    if (!g_fb.base) return false;
    cols = g_fb.width / PX_W;
    rows = g_fb.height / PX_H;
    if (cols > CON_MAX_COLS) cols = CON_MAX_COLS;
    if (rows > CON_MAX_ROWS) rows = CON_MAX_ROWS;
    if (!cols || !rows) return false;
    fg = fb_rgb(0xE0, 0xE6, 0xF0);
    bg = fb_rgb(0x10, 0x18, 0x28);
    active = true;
    con_clear();
    return true;
}

bool con_active(void) { return active; }

void con_set_color(uint32_t f, uint32_t b) { fg = f; bg = b; }

void con_clear(void) {
    if (!active) return;
    for (uint32_t r = 0; r < rows; r++)
        for (uint32_t c = 0; c < cols; c++) shadow[r][c] = (struct cell){ ' ', fg, bg };
    if (!suspended) fb_fill_rect(0, 0, g_fb.width, g_fb.height, bg);
    cx = cy = 0;
}

void con_get_size(uint32_t *c, uint32_t *r) { *c = cols; *r = rows; }
void con_get_cursor(uint32_t *c, uint32_t *r) { *c = cx; *r = cy; }

static const uint8_t *glyph_for(char ch) {
    unsigned char c = (unsigned char)ch;
    if (c < 0x20 || c > 0x7E) c = '?';
    return font5x8[c - 0x20];
}

/* Is font unit (u,v) of the cell lit for this glyph? (u in 0..CELL_W, v in 0..CELL_H) */
static bool cell_unit_on(const uint8_t *g, uint32_t u, uint32_t v) {
    if (u >= FONT_W || v < 1 || v > FONT_H) return false;
    return (g[v - 1] >> (FONT_W - 1 - u)) & 1;
}

static void paint_cell(uint32_t col, uint32_t row) {
    const struct cell *c = &shadow[row][col];
    const uint8_t *g = glyph_for(c->ch);
    uint32_t x0 = col * PX_W, y0 = row * PX_H;
    for (uint32_t v = 0; v < CELL_H; v++)
        for (uint32_t u = 0; u < CELL_W; u++)
            fb_fill_rect(x0 + u * CON_SCALE, y0 + v * CON_SCALE, CON_SCALE, CON_SCALE,
                         cell_unit_on(g, u, v) ? c->fg : c->bg);
}

static void draw_cell(uint32_t col, uint32_t row, char ch) {
    shadow[row][col] = (struct cell){ ch, fg, bg };
    if (!suspended) paint_cell(col, row);
}

void con_suspend(void) {
    uint64_t f = irq_save();
    suspended = true;
    irq_restore(f);
}

void con_resume(void) {
    uint64_t f = irq_save();
    if (suspended && active) {
        suspended = false;
        fb_fill_rect(0, 0, g_fb.width, g_fb.height, bg);
        for (uint32_t r = 0; r < rows; r++)
            for (uint32_t c = 0; c < cols; c++) paint_cell(c, r);
    }
    suspended = false;
    irq_restore(f);
}

bool con_suspended(void) { return suspended; }

bool con_cell_matches(uint32_t col, uint32_t row, char ch) {
    if (!active || col >= cols || row >= rows) return false;
    const uint8_t *g = glyph_for(ch);
    uint32_t x0 = col * PX_W, y0 = row * PX_H;
    for (uint32_t y = 0; y < PX_H; y++)
        for (uint32_t x = 0; x < PX_W; x++) {
            uint32_t want = cell_unit_on(g, x / CON_SCALE, y / CON_SCALE) ? fg : bg;
            if (fb_getpixel(x0 + x, y0 + y) != want) return false;
        }
    return true;
}

static void scroll(void) {
    memmove(shadow[0], shadow[1], sizeof shadow[0] * (rows - 1));
    for (uint32_t c = 0; c < cols; c++) shadow[rows - 1][c] = (struct cell){ ' ', fg, bg };
    if (suspended) return;
    fb_move_rows(0, PX_H, (rows - 1) * PX_H);
    fb_fill_rect(0, (rows - 1) * PX_H, g_fb.width, PX_H, bg);
}

static void newline(void) {
    cx = 0;
    if (++cy >= rows) { scroll(); cy = rows - 1; }
}

void con_putc(char c) {
    if (!active) return;
    switch (c) {
    case '\n': newline(); return;
    case '\r': cx = 0; return;
    case '\t': do con_putc(' '); while (cx % 4); return;
    case '\b': if (cx) { cx--; draw_cell(cx, cy, ' '); } return;
    default:
        draw_cell(cx, cy, c);
        if (++cx >= cols) newline();
    }
}

void console_write(const char *s) {
    serial_write(s);
    if (active) {
        uint64_t f = irq_save();      /* IRQ handlers echo too (tty) */
        while (*s) con_putc(*s++);
        irq_restore(f);
    }
}
