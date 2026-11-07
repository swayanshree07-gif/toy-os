/* Framebuffer access for user programs (Day 11 session).
 * One process at a time may own the screen. While it does, the text console keeps its
 * text only in its shadow buffer; when the owner releases it (or exits, faults or is
 * killed: proc_exit calls fbdev_release_by) the console repaints everything.
 * Programs draw into their own memory and copy rectangles in with fbblit, so the
 * kernel never maps the framebuffer into user space and can check every access. */
#include "fbdev.h"
#include "fb.h"
#include "console.h"
#include "idt.h"
#include "vmm.h"
#include "proc.h"
#include "string.h"
#include "syscall_nr.h"

static int owner;                 /* pid, 0 = console */
static uint64_t blits, blit_pixels;

int fbdev_owner(void) { return owner; }
void fbdev_stats(uint64_t *nblits, uint64_t *npixels) { *nblits = blits; *npixels = blit_pixels; }

int fbdev_info(struct tern_fbinfo *out) {
    if (!g_fb.base) return -ENODEV;
    out->width = g_fb.width;
    out->height = g_fb.height;
    out->owner = (uint32_t)owner;
    out->reserved = 0;
    return 0;
}

int fbdev_acquire(int pid) {
    if (!g_fb.base) return -ENODEV;
    uint64_t f = irq_save();
    int rc = 0;
    if (owner && owner != pid) rc = -EBUSY;
    else if (!owner) { owner = pid; con_suspend(); }
    irq_restore(f);
    return rc;
}

int fbdev_release_by(int pid) {
    uint64_t f = irq_save();
    int rc = -EPERM;
    if (owner && owner == pid) {
        owner = 0;
        rc = 0;
    }
    irq_restore(f);
    if (rc == 0) con_resume();
    return rc;
}

/* Native pixel from 0x00RRGGBB. */
static inline uint32_t native(uint32_t rgb) {
    return fb_rgb((uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb);
}

/* b and the pixel rows it names have been validated by the caller to be user memory
 * of the current process (checked here: geometry). */
int fbdev_blit(int pid, const struct tern_fbblit *b, uint64_t pml4) {
    if (!g_fb.base) return -ENODEV;
    if (owner != pid) return -EPERM;
    if (b->w == 0 || b->h == 0) return 0;
    if (b->x >= g_fb.width || b->y >= g_fb.height || b->w > g_fb.width - b->x ||
        b->h > g_fb.height - b->y)
        return -EINVAL;
    if (b->stride < 4ull * b->w || b->stride > (1u << 20) || ((uintptr_t)b->pixels & 3))
        return -EINVAL;
    uint64_t base = (uint64_t)(uintptr_t)b->pixels;
    uint64_t len = (uint64_t)b->stride * (b->h - 1) + 4ull * b->w;
    if (base + len < base || !vmm_user_range_ok(pml4, base, len, false)) return -EFAULT;
    bool same = g_fb.rpos == 16 && g_fb.gpos == 8 && g_fb.bpos == 0;
    for (uint32_t j = 0; j < b->h; j++) {
        /* the owner may have been changed by a kill between rows: stop drawing then */
        if (owner != pid) return -EPERM;
        const uint32_t *src = (const uint32_t *)(base + (uint64_t)j * b->stride);
        uint32_t *dst = (uint32_t *)(g_fb.base + (uint64_t)(b->y + j) * g_fb.pitch) + b->x;
        if (same) memcpy(dst, src, 4ull * b->w);
        else for (uint32_t i = 0; i < b->w; i++) dst[i] = native(src[i]);
    }
    blits++;
    blit_pixels += (uint64_t)b->w * b->h;
    return 0;
}
