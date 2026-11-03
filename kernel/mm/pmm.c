/* Physical memory manager: bitmap over 4 KiB frames, 1 bit per frame (1 = used).
 *
 * The bitmap covers frames 0 .. top of the highest usable memory-map region. Everything
 * starts out "used"; frames that lie entirely inside usable regions are then freed, and
 * reserved ranges (low 1 MiB, kernel image, Multiboot2 info, modules, the bitmap) are
 * marked used again. The bitmap itself lives in usable RAM, reached via the direct map. */
#include "pmm.h"
#include "mm.h"
#include "kernel.h"
#include "kprintf.h"
#include "string.h"
#include "idt.h"

#define MAX_RESERVED (4 + MB2_MAX_MODULES)

static uint64_t *bitmap;           /* virtual (direct map) */
static uint64_t nframes, nwords, free_count, hint;
static struct pmm_range reserved[MAX_RESERVED];
static unsigned nreserved;

static inline void set_bit(uint64_t f)   { bitmap[f >> 6] |= 1ull << (f & 63); }
static inline void clear_bit(uint64_t f) { bitmap[f >> 6] &= ~(1ull << (f & 63)); }
static inline bool test_bit(uint64_t f)  { return bitmap[f >> 6] >> (f & 63) & 1; }

static void add_reserved(uint64_t start, uint64_t end, const char *what) {
    kassert(nreserved < MAX_RESERVED);
    reserved[nreserved++] = (struct pmm_range){ ALIGN_DOWN(start, PAGE_SIZE),
                                                ALIGN_UP(end, PAGE_SIZE), what };
}

static bool overlaps_reserved(uint64_t start, uint64_t end) {
    for (unsigned i = 0; i < nreserved; i++)
        if (start < reserved[i].end && reserved[i].start < end) return true;
    return false;
}

/* Mark [start, end) used (used=true) or free; only whole frames, clipped to the bitmap. */
static void mark_range(uint64_t start, uint64_t end, bool used) {
    uint64_t f0 = used ? start >> PAGE_SHIFT : ALIGN_UP(start, PAGE_SIZE) >> PAGE_SHIFT;
    uint64_t f1 = used ? ALIGN_UP(end, PAGE_SIZE) >> PAGE_SHIFT : end >> PAGE_SHIFT;
    if (f1 > nframes) f1 = nframes;
    for (uint64_t f = f0; f < f1; f++) {
        if (used && !test_bit(f)) { set_bit(f); free_count--; }
        else if (!used && test_bit(f)) { clear_bit(f); free_count++; }
    }
}

#define FOR_EACH_MMAP(bi, e)                                                          \
    for (uint32_t _i = 0; _i < (bi)->mmap_count; _i++)                                \
        for (struct mb2_mmap_entry *e = (struct mb2_mmap_entry *)((uintptr_t)(bi)->mmap \
                 + _i * (bi)->mmap_entry_size); e; e = NULL)

void pmm_init(const struct boot_info *bi) {
    kassert(bi->mmap && bi->mmap_count);

    uint64_t top = 0;
    FOR_EACH_MMAP(bi, e)
        if (e->type == 1 && e->addr + e->len > top) top = e->addr + e->len;
    nframes = top >> PAGE_SHIFT;
    nwords = (nframes + 63) / 64;
    uint64_t bitmap_bytes = ALIGN_UP(nwords * 8, PAGE_SIZE);

    add_reserved(0, 0x100000, "low 1 MiB");
    add_reserved(virt_to_phys(__kernel_start), virt_to_phys(__kernel_end), "kernel");
    add_reserved(bi->info_phys, bi->info_phys + bi->info_size, "mb2 info");
    for (uint32_t i = 0; i < bi->module_count && i < MB2_MAX_MODULES; i++)
        add_reserved(bi->modules[i].start, bi->modules[i].end, "module");

    /* First fit for the bitmap inside a usable region, avoiding reserved ranges. */
    uint64_t where = 0;
    FOR_EACH_MMAP(bi, e) {
        if (e->type != 1 || where) continue;
        uint64_t cand = ALIGN_UP(e->addr, PAGE_SIZE), end = e->addr + e->len;
        while (cand + bitmap_bytes <= end) {
            bool clash = false;
            for (unsigned r = 0; r < nreserved; r++)
                if (cand < reserved[r].end && reserved[r].start < cand + bitmap_bytes) {
                    cand = reserved[r].end;
                    clash = true;
                }
            if (!clash) { where = cand; break; }
        }
    }
    if (!where) panic("pmm: no room for a %lu-byte bitmap", bitmap_bytes);
    add_reserved(where, where + bitmap_bytes, "pmm bitmap");
    bitmap = phys_to_virt(where);

    memset(bitmap, 0xFF, nwords * 8);       /* all used, including padding bits */
    free_count = 0;
    FOR_EACH_MMAP(bi, e)
        if (e->type == 1) mark_range(e->addr, e->addr + e->len, false);
    for (unsigned r = 0; r < nreserved; r++)
        mark_range(reserved[r].start, reserved[r].end, true);
    hint = 0;

    kprintf("pmm: %lu frames (%lu MiB) tracked, %lu free (%lu MiB), bitmap %lu KiB at %lx\n",
            nframes, nframes >> 8, free_count, free_count >> 8, bitmap_bytes >> 10, where);
    for (unsigned r = 0; r < nreserved; r++)
        kprintf("  reserved %016lx-%016lx %s\n", reserved[r].start, reserved[r].end - 1,
                reserved[r].what);
}

uint64_t pmm_alloc(void) {
    uint64_t fl = irq_save();
    for (uint64_t n = 0; n < nwords; n++) {
        uint64_t w = (hint + n) % nwords;
        if (bitmap[w] == ~0ull) continue;
        uint64_t f = w * 64 + (uint64_t)__builtin_ctzll(~bitmap[w]);
        set_bit(f);
        free_count--;
        hint = w;
        irq_restore(fl);
        return f << PAGE_SHIFT;
    }
    irq_restore(fl);
    return 0;
}

uint64_t pmm_alloc_zeroed(void) {
    uint64_t pa = pmm_alloc();
    if (pa) memset(phys_to_virt(pa), 0, PAGE_SIZE);
    return pa;
}

void pmm_free(uint64_t pa) {
    uint64_t f = pa >> PAGE_SHIFT;
    if ((pa & (PAGE_SIZE - 1)) || f >= nframes) panic("pmm_free: bad address %lx", pa);
    if (overlaps_reserved(pa, pa + PAGE_SIZE)) panic("pmm_free: reserved frame %lx", pa);
    uint64_t fl = irq_save();
    if (!test_bit(f)) panic("pmm_free: double free of %lx", pa);
    clear_bit(f);
    free_count++;
    if ((f >> 6) < hint) hint = f >> 6;
    irq_restore(fl);
}

bool pmm_is_used(uint64_t pa) {
    uint64_t f = pa >> PAGE_SHIFT;
    return f >= nframes || test_bit(f);
}

uint64_t pmm_free_frames(void) { return free_count; }
uint64_t pmm_total_frames(void) { return nframes; }

unsigned pmm_reserved(const struct pmm_range **out) {
    *out = reserved;
    return nreserved;
}
