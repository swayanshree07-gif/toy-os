/* User heap: malloc/free/calloc/realloc on top of brk. First fit over an address-
 * ordered free list; neighbouring free blocks are merged on free. Every block has a
 * 16-byte header (size incl. header, magic) and payloads are 16-byte aligned. */
#include "tern.h"

#define ALIGN 16
#define MAGIC_USED 0x7E3A11C5u
#define MAGIC_FREE 0x0F0EF7EEu
#define GROW_MIN (64 * 1024)

struct block {
    uint64_t size;            /* bytes including this header */
    uint32_t magic;
    uint32_t pad;
    struct block *next;       /* free list only (overlaps the payload) */
};
#define HDR 16

static struct block *free_list;
static uintptr_t heap_end;    /* current break */

static void die(const char *what, void *p) {
    dprintf(2, "malloc: %s %p\n", what, p);
    exit(134);
}

static void insert_free(struct block *b) {
    b->magic = MAGIC_FREE;
    struct block **pp = &free_list, *prev = NULL;
    while (*pp && *pp < b) { prev = *pp; pp = &(*pp)->next; }
    b->next = *pp;
    *pp = b;
    if (b->next && (uintptr_t)b + b->size == (uintptr_t)b->next) {      /* merge right */
        b->size += b->next->size;
        b->next = b->next->next;
    }
    if (prev && (uintptr_t)prev + prev->size == (uintptr_t)b) {        /* merge left */
        prev->size += b->size;
        prev->next = b->next;
    }
}

static bool grow(uint64_t need) {
    if (!heap_end) {
        long cur = brk(0);
        if (cur < 0) return false;
        heap_end = ((uintptr_t)cur + ALIGN - 1) & ~(uintptr_t)(ALIGN - 1);
        if (brk(heap_end) != (long)heap_end) return false;
    }
    uint64_t n = need < GROW_MIN ? GROW_MIN : (need + 4095) & ~4095ull;
    uintptr_t old = heap_end;
    if (brk(old + n) != (long)(old + n)) return false;
    heap_end = old + n;
    struct block *b = (struct block *)old;
    b->size = n;
    insert_free(b);
    return true;
}

void *malloc(size_t n) {
    if (n == 0) n = 1;
    if (n > USER_HEAP_MAX) return NULL;
    uint64_t need = (n + HDR + ALIGN - 1) & ~(uint64_t)(ALIGN - 1);
    for (int attempt = 0; attempt < 2; attempt++) {
        for (struct block **pp = &free_list; *pp; pp = &(*pp)->next) {
            struct block *b = *pp;
            if (b->magic != MAGIC_FREE) die("free list corrupted at", b);
            if (b->size < need) continue;
            if (b->size - need >= HDR + ALIGN) {            /* split */
                struct block *rest = (struct block *)((uintptr_t)b + need);
                rest->size = b->size - need;
                rest->magic = MAGIC_FREE;
                rest->next = b->next;
                *pp = rest;
                b->size = need;
            } else {
                *pp = b->next;
            }
            b->magic = MAGIC_USED;
            return (uint8_t *)b + HDR;
        }
        if (!grow(need)) return NULL;
    }
    return NULL;
}

void free(void *p) {
    if (!p) return;
    struct block *b = (struct block *)((uint8_t *)p - HDR);
    if (((uintptr_t)p & (ALIGN - 1)) || b->magic != MAGIC_USED) die("bad free of", p);
    insert_free(b);
}

void *calloc(size_t n, size_t sz) {
    if (sz && n > USER_HEAP_MAX / sz) return NULL;
    void *p = malloc(n * sz);
    if (p) memset(p, 0, n * sz);
    return p;
}

void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    struct block *b = (struct block *)((uint8_t *)p - HDR);
    if (b->magic != MAGIC_USED) die("bad realloc of", p);
    if (b->size - HDR >= n) return p;
    void *q = malloc(n);
    if (!q) return NULL;
    memcpy(q, p, b->size - HDR);
    free(p);
    return q;
}

/* Diagnostics for tests: number of free blocks and their total size. */
void malloc_stats(size_t *free_blocks, size_t *free_bytes) {
    size_t nb = 0, by = 0;
    for (struct block *b = free_list; b; b = b->next) { nb++; by += b->size; }
    *free_blocks = nb;
    *free_bytes = by;
}
