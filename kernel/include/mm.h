#ifndef TERN_MM_H
#define TERN_MM_H
/* Memory layout and physical/virtual address helpers.
 *
 * Virtual address map (48-bit canonical):
 *   0x0000000000000000 - 0x00007FFFFFFFFFFF   user space (Day 15+); unmapped in the kernel
 *   0xFFFF800000000000 - ...                  HHDM: direct map of physical memory
 *                                             (0 .. max(4 GiB, top of RAM), 2 MiB pages, NX)
 *   0xFFFFFF0000000000 - 0xFFFFFF0FFFFFFFFF   kernel dynamic mappings: tests below +1 GiB,
 *                                             thread stacks (guard page + 16 KiB) from +1 GiB
 *   0xFFFFFF1000000000 + 1 GiB max            kernel heap (kmalloc)
 *   0xFFFFFFFF80000000 + 1 MiB                kernel image (text RX, rodata R, data/bss RW)
 */
#include <stdint.h>
#include <stdbool.h>

#define PAGE_SIZE     4096ull
#define PAGE_SHIFT    12
#define KERNEL_VMA    0xFFFFFFFF80000000ull
#define HHDM_BASE     0xFFFF800000000000ull
#define KMAP_BASE     0xFFFFFF0000000000ull
#define KHEAP_BASE    0xFFFFFF1000000000ull   /* kernel heap, grows upwards */
#define KHEAP_MAX     (1ull << 30)

#define ALIGN_UP(x, a)   (((x) + (a) - 1) & ~((uint64_t)(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((uint64_t)(a) - 1))

static inline void *phys_to_virt(uint64_t pa) { return (void *)(pa + HHDM_BASE); }

/* Physical address of a kernel-image or direct-map pointer. */
static inline uint64_t virt_to_phys(const void *p) {
    uint64_t v = (uint64_t)p;
    return v >= KERNEL_VMA ? v - KERNEL_VMA : v - HHDM_BASE;
}

/* Linker-script symbols (virtual addresses of the kernel image). */
extern char __kernel_start[], __kernel_end[], __text_start[], __text_end[],
            __rodata_start[], __rodata_end[], __data_start[];

#endif
