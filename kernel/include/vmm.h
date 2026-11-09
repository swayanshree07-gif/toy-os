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
#define PTE_COW  (1ull << 9)     /* software bit: read-only because shared after fork */
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

/* ---- user address spaces (Day 15) ----
 * A user PML4 shares the kernel half (entries 256..511 point at the kernel's PDPTs);
 * the lower half belongs to the process and every frame mapped there is owned by it. */
#define USER_TOP 0x0000800000000000ull     /* first non-canonical lower-half address */
uint64_t vmm_new_space(void);              /* new PML4 (physical), 0 if out of memory */
/* Frees every user page, every lower-half page table and the PML4 itself.
 * Must not be the current CR3. */
void vmm_destroy_space(uint64_t pml4);
/* Allocate, zero and map `pages` fresh frames at va with flags (PTE_U is added). */
int vmm_map_user_pages(uint64_t pml4, uint64_t va, uint64_t pages, uint64_t flags);
/* True if [va, va+len) lies in user space and every page is mapped present+user
 * (and writable if `write`). len 0 is fine. */
bool vmm_user_range_ok(uint64_t pml4, uint64_t va, uint64_t len, bool write);
/* Number of pages mapped in the lower half (for tests/diagnostics). */
uint64_t vmm_user_pages(uint64_t pml4);

/* ---- copy-on-write (Day 22) ----
 * Clone the lower half of pml4: every present user page is shared (frame reference
 * +1); writable pages become read-only + PTE_COW in both spaces. Returns the new PML4
 * or 0 if out of memory (nothing leaks; the parent keeps working). */
uint64_t vmm_fork_space(uint64_t pml4);
/* Resolve a write to a PTE_COW page: copy the frame (or just make it writable again
 * if this space is its last owner). 1 handled, 0 not a COW page, -1 out of memory. */
int vmm_cow_fault(uint64_t pml4, uint64_t va);
struct vmm_cow_stats { uint64_t forks, shared_pages, copies, reuses; };
const struct vmm_cow_stats *vmm_cow_stats(void);

static inline void write_cr3(uint64_t v) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory");
}
static inline uint64_t read_cr3(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}
static inline void invlpg(uint64_t va) {
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
}

#endif
