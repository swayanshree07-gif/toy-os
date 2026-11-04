#ifndef TERN_VMM_H
#define TERN_VMM_H
/* Virtual memory: 4-level x86-64 page tables built by the kernel itself. */
#include <stdint.h>
#include <stdbool.h>
#include "multiboot2.h"

#define PTE_P    (1ull << 0)
#define PTE_W    (1ull << 1)
#define PTE_U    (1ull << 2)
#define PTE_PWT  (1ull << 3)
#define PTE_PCD  (1ull << 4)
#define PTE_A    (1ull << 5)
#define PTE_D    (1ull << 6)
#define PTE_PS   (1ull << 7)
#define PTE_G    (1ull << 8)
#define PTE_NX   (1ull << 63)
#define PTE_ADDR 0x000FFFFFFFFFF000ull

/* Flags for vmm_map (PTE_P is implied). */
#define VMM_KERNEL_RW  (PTE_W | PTE_NX | PTE_G)
#define VMM_KERNEL_RX  (PTE_G)

void vmm_init(const struct boot_info *bi);
uint64_t vmm_kernel_pml4(void);        /* physical address of the kernel PML4 */

/* Map one 4 KiB page; allocates intermediate tables. 0 ok, -1 already mapped or
 * a large page in the way, -2 out of memory. */
int vmm_map(uint64_t pml4, uint64_t va, uint64_t pa, uint64_t flags);
/* Unmap one 4 KiB page; returns true and the old physical address if it was mapped. */
bool vmm_unmap(uint64_t pml4, uint64_t va, uint64_t *old_pa);
/* Translate a virtual address (any page size); flags gets the leaf entry's flags. */
bool vmm_translate(uint64_t pml4, uint64_t va, uint64_t *pa, uint64_t *flags);

/* Map device registers (uncached, NX) into the kernel MMIO window; NULL on failure. */
void *vmm_map_mmio(uint64_t pa, uint64_t size);

static inline uint64_t read_cr3(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}
static inline void invlpg(uint64_t va) {
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
}

#endif
