#ifndef TERN_FB_H
#define TERN_FB_H
#include <stdint.h>
#include <stdbool.h>
#include "multiboot2.h"

struct framebuffer {
    uint8_t *base;
    uint32_t width, height, pitch, bpp;
    uint8_t rpos, gpos, bpos;
};

extern struct framebuffer g_fb;

bool fb_init(const struct mb2_tag_framebuffer *tag);   /* 32-bpp direct RGB only */
uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b);
void fb_putpixel(uint32_t x, uint32_t y, uint32_t color);
uint32_t fb_getpixel(uint32_t x, uint32_t y);
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
/* Move pixel rows [src_y, src_y+h) to dst_y (overlap-safe). */
void fb_move_rows(uint32_t dst_y, uint32_t src_y, uint32_t h);

#endif
