#ifndef TERN_HEAP_H
#define TERN_HEAP_H
/* Kernel heap: kmalloc/kfree with boundary tags, an explicit free list, coalescing,
 * and on-demand growth by mapping fresh frames at KHEAP_BASE. */
#include <stddef.h>
#include <stdint.h>

void heap_init(void);
void *kmalloc(size_t n);            /* 16-byte aligned, NULL if out of memory */
void *kzalloc(size_t n);
void kfree(void *p);                /* NULL ok; panics on double free / corruption */

struct heap_stats {
    uint64_t mapped, used_bytes, free_bytes;
    uint64_t used_blocks, free_blocks;
};
/* Walks every block, verifies headers/footers/free list; panics on corruption. */
void heap_check(struct heap_stats *out);

void test_heap(void);

#endif
