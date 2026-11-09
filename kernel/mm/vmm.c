/* Virtual memory manager: the kernel's own 4-level page tables.
 *
 * vmm_init() replaces the boot page tables (boot.S) with:
 *   - a direct map (HHDM) of physical memory 0 .. max(4 GiB, top of RAM/framebuffer)
 *     using 2 MiB pages, read/write, no-execute, global;
 *   - the kernel image at KERNEL_VMA with 4 KiB pages and per-section permissions
 *     (text read+execute, rodata read-only+NX, data/bss read/write+NX);
 *   - nothing in the lower half: the identity map is gone, so NULL dereferences fault.
 * All 256 kernel-half PML4 slots get a PDPT up front, so future per-process address
 * spaces can share the kernel half by copying PML4 entries 256..511. */
#include "vmm.h"
#include "pmm.h"
#include "mm.h"
#include "kernel.h"
#include "kprintf.h"
#include "idt.h"
#include "string.h"

static uint64_t kernel_pml4;
static uint64_t nx_mask;           /* PTE_NX if the CPU supports it, else 0 */

uint64_t vmm_kernel_pml4(void) { return kernel_pml4; }

static inline uint64_t *table_of(uint64_t entry) {
    return phys_to_virt(entry & PTE_ADDR);
}

/* Walk to the entry that maps va at `leaf` level (0 = 4 KiB PT entry, 1 = 2 MiB PD entry),
 * creating intermediate tables if asked. Returns NULL with *err set on failure. */
static uint64_t *walk(uint64_t pml4, uint64_t va, int leaf, bool create, uint64_t uflag, int *err) {
    uint64_t *table = phys_to_virt(pml4);
    for (int level = 3; level > leaf; level--) {
        uint64_t *e = &table[(va >> (PAGE_SHIFT + 9 * level)) & 511];
        if (!(*e & PTE_P)) {
            if (!create) { *err = -1; return NULL; }
            uint64_t pa = pmm_alloc_zeroed();
            if (!pa) { *err = -2; return NULL; }
            *e = pa | PTE_P | PTE_W | uflag;
        } else if (*e & PTE_PS) {
            *err = -1;
            return NULL;
        } else {
            *e |= uflag;
        }
        table = table_of(*e);
    }
    return &table[(va >> (PAGE_SHIFT + 9 * leaf)) & 511];
}

static bool is_current(uint64_t pml4) { return (read_cr3() & PTE_ADDR) == pml4; }

int vmm_map(uint64_t pml4, uint64_t va, uint64_t pa, uint64_t flags) {
    kassert(!(va & (PAGE_SIZE - 1)) && !(pa & (PAGE_SIZE - 1)));
    int err = 0;
    uint64_t fl = irq_save();
    uint64_t *pte = walk(pml4, va, 0, true, flags & PTE_U, &err);
    if (pte && (*pte & PTE_P)) err = -1;
    if (!err) {
        *pte = pa | PTE_P | (flags & ~(PTE_ADDR | PTE_PS | PTE_NX)) | (flags & nx_mask);
        if (is_current(pml4)) invlpg(va);
    }
    irq_restore(fl);
    return err;
}

bool vmm_unmap(uint64_t pml4, uint64_t va, uint64_t *old_pa) {
    int err = 0;
    bool ok = false;
    uint64_t fl = irq_save();
    uint64_t *pte = walk(pml4, va, 0, false, 0, &err);
    if (pte && (*pte & PTE_P)) {
        if (old_pa) *old_pa = *pte & PTE_ADDR;
        *pte = 0;
        if (is_current(pml4)) invlpg(va);
        ok = true;
    }
    irq_restore(fl);
    return ok;
}

bool vmm_translate(uint64_t pml4, uint64_t va, uint64_t *pa, uint64_t *flags) {
    uint64_t *table = phys_to_virt(pml4);
    for (int level = 3; level >= 0; level--) {
        uint64_t e = table[(va >> (PAGE_SHIFT + 9 * level)) & 511];
        if (!(e & PTE_P)) return false;
        if (level == 0 || (level <= 2 && (e & PTE_PS))) {
            uint64_t size = 1ull << (PAGE_SHIFT + 9 * level);
            uint64_t base = e & PTE_ADDR & ~(size - 1);
            if (pa) *pa = base + (va & (size - 1));
            if (flags) *flags = e & ~PTE_ADDR;
            return true;
        }
        table = table_of(e);
    }
    return false;
}

static void map_2m(uint64_t pml4, uint64_t va, uint64_t pa, uint64_t flags) {
    int err = 0;
    uint64_t *pde = walk(pml4, va, 1, true, 0, &err);
    if (!pde || (*pde & PTE_P)) panic("vmm: map_2m %lx failed (%d)", va, err);
    *pde = pa | PTE_P | PTE_PS | flags;
}

static void cpu_enable_nx_pge(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000001), "c"(0));
    if (d & (1u << 20)) {
        uint32_t lo, hi;
        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080));
        lo |= 1u << 11;                                /* EFER.NXE */
        __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(0xC0000080));
        nx_mask = PTE_NX;
    }
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= 1ull << 7;                                  /* CR4.PGE: global pages */
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
}

void vmm_init(const struct boot_info *bi) {
    cpu_enable_nx_pge();

    kernel_pml4 = pmm_alloc_zeroed();
    if (!kernel_pml4) panic("vmm: no memory for PML4");
    uint64_t *pml4 = phys_to_virt(kernel_pml4);
    for (int i = 256; i < 512; i++) {
        uint64_t pdpt = pmm_alloc_zeroed();
        if (!pdpt) panic("vmm: no memory for kernel PDPTs");
        pml4[i] = pdpt | PTE_P | PTE_W;
    }

    /* Direct map. */
    uint64_t top = 1ull << 32;
    for (uint32_t i = 0; i < bi->mmap_count; i++) {
        struct mb2_mmap_entry *e =
            (struct mb2_mmap_entry *)((uintptr_t)bi->mmap + i * bi->mmap_entry_size);
        if ((e->type == 1 || e->type == 3 || e->type == 4) && e->addr + e->len > top)
            top = e->addr + e->len;
    }
    if (bi->fb) {
        uint64_t fb_end = bi->fb->addr + (uint64_t)bi->fb->pitch * bi->fb->height;
        if (fb_end > top) top = fb_end;
    }
    top = ALIGN_UP(top, 2ull << 20);
    for (uint64_t pa = 0; pa < top; pa += 2ull << 20)
        map_2m(kernel_pml4, HHDM_BASE + pa, pa, PTE_W | PTE_G | nx_mask);

    /* Kernel image, page by page with section permissions. */
    for (char *va = __kernel_start; va < __kernel_end; va += PAGE_SIZE) {
        uint64_t flags;
        if (va < __text_end) flags = PTE_G;                           /* RX */
        else if (va < __rodata_end) flags = PTE_G | PTE_NX;           /* R */
        else flags = PTE_G | PTE_W | PTE_NX;                          /* RW */
        if (vmm_map(kernel_pml4, (uint64_t)va, virt_to_phys(va), flags))
            panic("vmm: mapping kernel page %p failed", va);
    }

    __asm__ volatile("mov %0, %%cr3" : : "r"(kernel_pml4) : "memory");
    kprintf("vmm: kernel PML4 at %lx, direct map %lu MiB, nx %s\n", kernel_pml4, top >> 20,
            nx_mask ? "on" : "off");
}

/* ---- device MMIO: uncached 4 KiB mappings in KMAP_BASE + 512 MiB .. +1 GiB ---- */
#define MMIO_AREA (KMAP_BASE + (512ull << 20))
#define MMIO_END  (KMAP_BASE + (1ull << 30))
static uint64_t mmio_next = MMIO_AREA;

void *vmm_map_mmio(uint64_t pa, uint64_t size) {
    uint64_t off = pa & (PAGE_SIZE - 1);
    uint64_t pages = ALIGN_UP(off + size, PAGE_SIZE) / PAGE_SIZE;
    if (mmio_next + pages * PAGE_SIZE > MMIO_END) return NULL;
    uint64_t va = mmio_next;
    for (uint64_t i = 0; i < pages; i++)
        if (vmm_map(kernel_pml4, va + i * PAGE_SIZE, ALIGN_DOWN(pa, PAGE_SIZE) + i * PAGE_SIZE,
                    VMM_KERNEL_RW | PTE_PCD | PTE_PWT))
            return NULL;
    mmio_next += pages * PAGE_SIZE;
    return (void *)(va + off);
}

/* ---- user address spaces ---- */
uint64_t vmm_new_space(void) {
    uint64_t pml4 = pmm_alloc_zeroed();
    if (!pml4) return 0;
    uint64_t *dst = phys_to_virt(pml4), *src = phys_to_virt(kernel_pml4);
    for (int i = 256; i < 512; i++) dst[i] = src[i];
    return pml4;
}

/* Free a lower-half table at `level` (2 = PDPT, 1 = PD, 0 = PT) and everything below. */
static void free_table(uint64_t table_pa, int level) {
    uint64_t *t = phys_to_virt(table_pa);
    for (int i = 0; i < 512; i++) {
        if (!(t[i] & PTE_P)) continue;
        kassert(!(t[i] & PTE_PS));         /* user space only uses 4 KiB pages */
        if (level > 0) free_table(t[i] & PTE_ADDR, level - 1);
        else pmm_free(t[i] & PTE_ADDR);
    }
    pmm_free(table_pa);
}

void vmm_destroy_space(uint64_t pml4) {
    kassert(pml4 != kernel_pml4 && !is_current(pml4));
    uint64_t *t = phys_to_virt(pml4);
    for (int i = 0; i < 256; i++)
        if (t[i] & PTE_P) free_table(t[i] & PTE_ADDR, 2);
    pmm_free(pml4);
}

int vmm_map_user_pages(uint64_t pml4, uint64_t va, uint64_t pages, uint64_t flags) {
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t a = va + i * PAGE_SIZE;
        if (a >= USER_TOP || a < va) return -1;
        uint64_t pa = pmm_alloc_zeroed();
        if (!pa) return -2;
        int rc = vmm_map(pml4, a, pa, flags | PTE_U);
        if (rc) { pmm_free(pa); return rc; }
    }
    return 0;
}

bool vmm_user_range_ok(uint64_t pml4, uint64_t va, uint64_t len, bool write) {
    if (len == 0) return true;
    if (va >= USER_TOP || len > USER_TOP - va) return false;
    for (uint64_t p = ALIGN_DOWN(va, PAGE_SIZE); p < va + len; p += PAGE_SIZE) {
        uint64_t fl;
        if (!vmm_translate(pml4, p, NULL, &fl)) return false;
        if (!(fl & PTE_U) || (write && !(fl & PTE_W))) return false;
    }
    return true;
}

static uint64_t count_table(uint64_t table_pa, int level) {
    uint64_t *t = phys_to_virt(table_pa), n = 0;
    for (int i = 0; i < 512; i++)
        if (t[i] & PTE_P) n += level ? count_table(t[i] & PTE_ADDR, level - 1) : 1;
    return n;
}

uint64_t vmm_user_pages(uint64_t pml4) {
    uint64_t *t = phys_to_virt(pml4), n = 0;
    for (int i = 0; i < 256; i++)
        if (t[i] & PTE_P) n += count_table(t[i] & PTE_ADDR, 2);
    return n;
}

/* ---- copy-on-write fork (Day 22) ---- */
static struct vmm_cow_stats cow_stats;
const struct vmm_cow_stats *vmm_cow_stats(void) { return &cow_stats; }

static int fork_table(uint64_t *t, int level, uint64_t base, uint64_t child) {
    for (int i = 0; i < 512; i++) {
        if (!(t[i] & PTE_P)) continue;
        uint64_t va = base + ((uint64_t)i << (PAGE_SHIFT + 9 * level));
        if (level > 0) {
            int rc = fork_table(table_of(t[i]), level - 1, va, child);
            if (rc) return rc;
            continue;
        }
        if (t[i] & PTE_W) t[i] = (t[i] & ~PTE_W) | PTE_COW;
        uint64_t pa = t[i] & PTE_ADDR;
        uint64_t fl = t[i] & (PTE_U | PTE_COW | PTE_NX);
        int rc = vmm_map(child, va, pa, fl);
        if (rc) return rc;
        pmm_ref(pa);
        cow_stats.shared_pages++;
    }
    return 0;
}

uint64_t vmm_fork_space(uint64_t pml4) {
    uint64_t child = vmm_new_space();
    if (!child) return 0;
    uint64_t fl = irq_save();
    uint64_t *t = phys_to_virt(pml4);
    int rc = 0;
    for (int i = 0; i < 256 && !rc; i++)
        if (t[i] & PTE_P) rc = fork_table(table_of(t[i]), 2, (uint64_t)i << 39, child);
    if (is_current(pml4)) write_cr3(read_cr3());      /* parent lost PTE_W: flush */
    irq_restore(fl);
    if (rc) { vmm_destroy_space(child); return 0; }
    cow_stats.forks++;
    return child;
}

int vmm_cow_fault(uint64_t pml4, uint64_t va) {
    if (va >= USER_TOP) return 0;
    va = va & ~(uint64_t)(PAGE_SIZE - 1);
    int err = 0, rc = 0;
    uint64_t fl = irq_save();
    uint64_t *pte = walk(pml4, va, 0, false, 0, &err);
    if (pte && (*pte & PTE_P) && (*pte & PTE_COW)) {
        uint64_t old = *pte & PTE_ADDR;
        if (pmm_refcount(old) == 1) {             /* the others are gone: keep it */
            *pte = (*pte | PTE_W) & ~PTE_COW;
            cow_stats.reuses++;
            rc = 1;
        } else {
            uint64_t pa = pmm_alloc();
            if (!pa) rc = -1;
            else {
                memcpy(phys_to_virt(pa), phys_to_virt(old), PAGE_SIZE);
                *pte = pa | ((*pte & ~PTE_ADDR & ~PTE_COW) | PTE_W);
                pmm_free(old);                    /* drops our reference only */
                cow_stats.copies++;
                rc = 1;
            }
        }
        if (rc == 1 && is_current(pml4)) invlpg(va);
    }
    irq_restore(fl);
    return rc;
}
