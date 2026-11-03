/* Kernel heap.
 *
 * The heap is one contiguous virtual range [KHEAP_BASE, heap_end) carved into blocks.
 * Every block starts with a 16-byte header {size | USED, magic} and ends with an 8-byte
 * footer holding a copy of size|USED (boundary tags), so both neighbours of a block can
 * be found in O(1) and freed blocks are merged immediately. Free blocks are kept on a
 * doubly linked list stored in their payload; allocation is first fit with splitting.
 * When nothing fits, the heap grows by mapping fresh frames at heap_end. */
#include "heap.h"
#include "mm.h"
#include "pmm.h"
#include "vmm.h"
#include "idt.h"
#include "kernel.h"
#include "kprintf.h"
#include "string.h"

#define USED        1ull
#define MAGIC       0x7E4E4EA9B10C0000ull
#define HDR         16ull
#define OVERHEAD    32ull           /* header + 16-byte trailer (footer in its last 8) */
#define MIN_BLOCK   48ull           /* header + free-list links + trailer */
#define GROW_MIN    (64ull << 10)

struct block {
    uint64_t size;                  /* total bytes incl. overhead; bit 0 = USED */
    uint64_t magic;
    struct block *next, *prev;      /* free blocks only (overlaps the payload) */
};

static uint64_t heap_end = KHEAP_BASE;
static struct block *free_list;

static inline uint64_t bsize(const struct block *b) { return b->size & ~USED; }
static inline bool bused(const struct block *b) { return b->size & USED; }
static inline uint64_t *footer(struct block *b) {
    return (uint64_t *)((char *)b + bsize(b) - 8);
}
static inline struct block *next_block(struct block *b) {
    return (struct block *)((char *)b + bsize(b));
}

static void set_block(struct block *b, uint64_t size, bool used) {
    b->size = size | (used ? USED : 0);
    b->magic = MAGIC;
    *footer(b) = b->size;
}

static void list_remove(struct block *b) {
    if (b->prev) b->prev->next = b->next; else free_list = b->next;
    if (b->next) b->next->prev = b->prev;
}

static void list_push(struct block *b) {
    b->prev = NULL;
    b->next = free_list;
    if (free_list) free_list->prev = b;
    free_list = b;
}

static void check_block(struct block *b, const char *who) {
    if ((uint64_t)b < KHEAP_BASE || (uint64_t)b >= heap_end || ((uint64_t)b & 15) ||
        b->magic != MAGIC || bsize(b) < MIN_BLOCK || (bsize(b) & 15) ||
        (uint64_t)b + bsize(b) > heap_end || *footer(b) != b->size)
        panic("%s: heap corruption at block %p (size %lx magic %lx)", who, b, b->size,
              (uint64_t)b >= KHEAP_BASE && (uint64_t)b < heap_end ? b->magic : 0);
}

/* Merge a free (not listed) block with free neighbours, then list it. */
static void insert_free(struct block *b) {
    uint64_t size = bsize(b);
    struct block *n = next_block(b);
    if ((uint64_t)n < heap_end && !bused(n)) {
        list_remove(n);
        size += bsize(n);
    }
    if ((uint64_t)b > KHEAP_BASE) {
        uint64_t prev_size = *((uint64_t *)b - 1);
        struct block *p = (struct block *)((char *)b - (prev_size & ~USED));
        if (!(prev_size & USED)) {
            check_block(p, "heap merge");
            list_remove(p);
            size += bsize(p);
            b = p;
        }
    }
    set_block(b, size, false);
    list_push(b);
}

static bool grow(uint64_t need) {
    uint64_t bytes = ALIGN_UP(need > GROW_MIN ? need : GROW_MIN, PAGE_SIZE);
    if (heap_end + bytes > KHEAP_BASE + KHEAP_MAX) return false;
    uint64_t k = vmm_kernel_pml4();
    for (uint64_t off = 0; off < bytes; off += PAGE_SIZE) {
        uint64_t pa = pmm_alloc();
        if (!pa || vmm_map(k, heap_end + off, pa, VMM_KERNEL_RW)) {
            if (pa) pmm_free(pa);
            while (off) {                         /* roll back */
                off -= PAGE_SIZE;
                uint64_t old;
                if (vmm_unmap(k, heap_end + off, &old)) pmm_free(old);
            }
            return false;
        }
    }
    struct block *b = (struct block *)heap_end;
    heap_end += bytes;
    set_block(b, bytes, false);
    insert_free(b);
    return true;
}

void heap_init(void) {
    if (!grow(GROW_MIN)) panic("heap: cannot map initial heap");
    kprintf("heap: at %lx, %lu KiB mapped\n", (uint64_t)KHEAP_BASE, (uint64_t)(heap_end - KHEAP_BASE) >> 10);
}

void *kmalloc(size_t n) {
    if (n == 0 || n > KHEAP_MAX) return NULL;
    uint64_t need = ALIGN_UP((uint64_t)n, 16) + OVERHEAD;
    uint64_t fl = irq_save();
    for (int attempt = 0; attempt < 2; attempt++) {
        for (struct block *b = free_list; b; b = b->next) {
            if (bsize(b) < need) continue;
            list_remove(b);
            uint64_t rest = bsize(b) - need;
            if (rest >= MIN_BLOCK) {
                set_block(b, need, true);
                struct block *r = next_block(b);
                set_block(r, rest, false);
                list_push(r);                     /* its neighbours are both in use */
            } else {
                set_block(b, bsize(b), true);
            }
            irq_restore(fl);
            return (char *)b + HDR;
        }
        if (attempt == 0 && !grow(need)) break;
    }
    irq_restore(fl);
    return NULL;
}

void *kzalloc(size_t n) {
    void *p = kmalloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void kfree(void *p) {
    if (!p) return;
    struct block *b = (struct block *)((char *)p - HDR);
    uint64_t fl = irq_save();
    check_block(b, "kfree");
    if (!bused(b)) panic("kfree: double free of %p", p);
    set_block(b, bsize(b), false);
    insert_free(b);
    irq_restore(fl);
}

void heap_check(struct heap_stats *out) {
    struct heap_stats s = { .mapped = heap_end - KHEAP_BASE };
    uint64_t fl = irq_save();
    bool prev_free = false;
    for (struct block *b = (struct block *)KHEAP_BASE; (uint64_t)b < heap_end; b = next_block(b)) {
        check_block(b, "heap_check");
        if (bused(b)) { s.used_blocks++; s.used_bytes += bsize(b); prev_free = false; }
        else {
            if (prev_free) panic("heap_check: adjacent free blocks at %p", b);
            s.free_blocks++;
            s.free_bytes += bsize(b);
            prev_free = true;
        }
    }
    uint64_t listed = 0;
    for (struct block *b = free_list, *prev = NULL; b; prev = b, b = b->next) {
        check_block(b, "heap_check list");
        if (bused(b) || b->prev != prev) panic("heap_check: bad free list at %p", b);
        if (++listed > s.free_blocks) break;
    }
    if (listed != s.free_blocks)
        panic("heap_check: %lu blocks listed, %lu free", listed, s.free_blocks);
    if (s.used_bytes + s.free_bytes != s.mapped) panic("heap_check: sizes don't add up");
    irq_restore(fl);
    if (out) *out = s;
}

/* ---- self-test: randomised alloc/free with fill patterns as canaries ---- */
static uint64_t rng_state = 0x7E4E5EEDull;
static uint64_t rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

#define SLOTS 128
void test_heap(void) {
    static uint8_t *ptr[SLOTS];
    static size_t len[SLOTS];
    struct heap_stats s0, s;
    heap_check(&s0);

    for (int op = 0; op < 4000; op++) {
        unsigned i = rng() % SLOTS;
        if (ptr[i]) {
            uint8_t pat = (uint8_t)(i * 31 + len[i]);
            for (size_t j = 0; j < len[i]; j++)
                if (ptr[i][j] != (uint8_t)(pat + j))
                    panic("heap: canary broken in slot %u at byte %lu", i, (uint64_t)j);
            kfree(ptr[i]);
            ptr[i] = NULL;
        } else {
            uint64_t r = rng();
            size_t n = (r & 15) == 0 ? 1 + r % 40000 : 1 + r % 600;   /* some big ones */
            uint8_t *p = kmalloc(n);
            if (!p || ((uint64_t)p & 15) || (uint64_t)p < KHEAP_BASE)
                panic("heap: kmalloc(%lu) returned %p", (uint64_t)n, p);
            uint8_t pat = (uint8_t)(i * 31 + n);
            for (size_t j = 0; j < n; j++) p[j] = (uint8_t)(pat + j);
            ptr[i] = p;
            len[i] = n;
        }
        if (op % 250 == 0) heap_check(NULL);
    }
    heap_check(&s);
    uint64_t live = s.used_blocks;
    for (unsigned i = 0; i < SLOTS; i++) { kfree(ptr[i]); ptr[i] = NULL; }
    heap_check(&s);
    if (s.used_blocks != s0.used_blocks || s.free_blocks != 1)
        panic("heap: after freeing all: %lu used, %lu free blocks", s.used_blocks, s.free_blocks);

    uint32_t *z = kzalloc(1000);
    for (int j = 0; j < 250; j++) if (z[j]) panic("heap: kzalloc not zeroed");
    kfree(z);
    if (kmalloc(0) != NULL) panic("heap: kmalloc(0) not NULL");
    kprintf("[TEST] heap ok: 4000 random ops, %lu live at end, %lu KiB mapped, fully coalesced\n",
            live, s.mapped >> 10);
}
