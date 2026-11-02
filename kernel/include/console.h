#ifndef TERN_CONSOLE_H
#define TERN_CONSOLE_H
#include <stdint.h>
#include <stdbool.h>

/* Text console drawn on the framebuffer with the built-in 5x8 font, scaled. */
#define FONT_W 5
#define FONT_H 8
#define CELL_W 6        /* font units incl. 1 column of spacing */
#define CELL_H 10       /* font units incl. 1 row above and below */
#define CON_SCALE 2

extern const uint8_t font5x8[95][8];

bool con_init(void);                    /* requires fb_init() first */
bool con_active(void);
void con_putc(char c);
void con_set_color(uint32_t fg, uint32_t bg);
void con_clear(void);
void con_get_size(uint32_t *cols, uint32_t *rows);
void con_get_cursor(uint32_t *col, uint32_t *row);
/* True if the pixels of cell (col,row) exactly show glyph ch in the current colours. */
bool con_cell_matches(uint32_t col, uint32_t row, char ch);

/* Write to every active output (serial + framebuffer console). */
void console_write(const char *s);

#endif
