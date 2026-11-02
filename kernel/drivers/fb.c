/* Linear framebuffer (from the Multiboot2 framebuffer tag, UEFI GOP underneath). */
#include "fb.h"
#include "string.h"

struct framebuffer g_fb;

bool fb_init(const struct mb2_tag_framebuffer *tag) {
    if (!tag || tag->fb_type != 1 || tag->bpp != 32) return false;
    /* Identity-mapped for now (boot page tables cover the first 4 GiB). */
    if (tag->addr + (uint64_t)tag->pitch * tag->height > (1ull << 32)) return false;
    g_fb.base = (uint8_t *)(uintptr_t)tag->addr;
    g_fb.width = tag->width;
    g_fb.height = tag->height;
    g_fb.pitch = tag->pitch;
    g_fb.bpp = tag->bpp;
    g_fb.rpos = tag->red_pos;
    g_fb.gpos = tag->green_pos;
    g_fb.bpos = tag->blue_pos;
    return true;
}

uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << g_fb.rpos) | ((uint32_t)g << g_fb.gpos) | ((uint32_t)b << g_fb.bpos);
}

static inline uint32_t *pixel_at(uint32_t x, uint32_t y) {
    return (uint32_t *)(g_fb.base + (uintptr_t)y * g_fb.pitch + (uintptr_t)x * 4);
}

void fb_putpixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x < g_fb.width && y < g_fb.height) *pixel_at(x, y) = color;
}

uint32_t fb_getpixel(uint32_t x, uint32_t y) {
    if (x < g_fb.width && y < g_fb.height) return *pixel_at(x, y);
    return 0;
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (x >= g_fb.width || y >= g_fb.height) return;
    if (w > g_fb.width - x) w = g_fb.width - x;
    if (h > g_fb.height - y) h = g_fb.height - y;
    for (uint32_t j = 0; j < h; j++) {
        uint32_t *p = pixel_at(x, y + j);
        for (uint32_t i = 0; i < w; i++) p[i] = color;
    }
}

void fb_move_rows(uint32_t dst_y, uint32_t src_y, uint32_t h) {
    if (src_y >= g_fb.height || dst_y >= g_fb.height) return;
    uint32_t max = g_fb.height - (src_y > dst_y ? src_y : dst_y);
    if (h > max) h = max;
    memmove(g_fb.base + (uintptr_t)dst_y * g_fb.pitch,
            g_fb.base + (uintptr_t)src_y * g_fb.pitch, (size_t)h * g_fb.pitch);
}
