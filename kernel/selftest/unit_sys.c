/* Unit tests: memory managers, MMIO mappings, ACPI and interrupt routing, sync. */
#include <stdint.h>
#include "ktest.h"
#include "string.h"
#include "mm.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "acpi.h"
#include "irq.h"
#include "pic.h"
#include "timer.h"
#include "sched.h"
#include "sync.h"

KTEST(pmm_alloc_free) {
    uint64_t before = pmm_free_frames();
    uint64_t a = pmm_alloc(), b = pmm_alloc_zeroed();
    KEXPECT(a && b && a != b);
    KEXPECT((a & (PAGE_SIZE - 1)) == 0 && (b & (PAGE_SIZE - 1)) == 0);
    KEXPECT(pmm_is_used(a) && pmm_is_used(b));
    const uint64_t *z = phys_to_virt(b);
    bool zero = true;
    for (int i = 0; i < 512; i++) if (z[i]) zero = false;
    KEXPECT(zero);
    KEXPECT(pmm_free_frames() == before - 2);
    pmm_free(a);
    pmm_free(b);
    KEXPECT(!pmm_is_used(a) && pmm_free_frames() == before);
}

KTEST(heap_basic) {
    struct heap_stats s0, s1;
    heap_check(&s0);
    char *p = kmalloc(100), *q = kzalloc(3000);
    KEXPECT(p && q && ((uint64_t)p & 15) == 0 && ((uint64_t)q & 15) == 0);
    bool zero = true;
    for (int i = 0; i < 3000; i++) if (q[i]) zero = false;
    KEXPECT(zero);
    memset(p, 0xAB, 100);
    KEXPECT(kmalloc(1ull << 40) == NULL);       /* way beyond the heap limit */
    kfree(p);
    kfree(q);
    kfree(NULL);
    heap_check(&s1);
    KEXPECT(s1.used_blocks == s0.used_blocks && s1.used_bytes == s0.used_bytes);
}

KTEST(vmm_mmio_uncached) {
    uint64_t pa = pmm_alloc_zeroed();
    KEXPECT(pa);
    if (!pa) return;
    volatile uint32_t *dm = phys_to_virt(pa);
    volatile uint32_t *io = vmm_map_mmio(pa + 0x10, 8);
    KEXPECT(io && ((uint64_t)io & 0xFFF) == 0x10);
    if (!io) { pmm_free(pa); return; }
    uint64_t got, flags;
    KEXPECT(vmm_translate(vmm_kernel_pml4(), (uint64_t)io, &got, &flags) && got == pa + 0x10);
    KEXPECT((flags & (PTE_PCD | PTE_PWT | PTE_NX)) == (PTE_PCD | PTE_PWT | PTE_NX));
    io[0] = 0x7E57ACE5;
    KEXPECT(dm[4] == 0x7E57ACE5);               /* same frame through the direct map */
    uint64_t old;
    KEXPECT(vmm_unmap(vmm_kernel_pml4(), (uint64_t)io & ~0xFFFull, &old) && old == pa);
    pmm_free(pa);
}

KTEST(acpi_tables) {
    if (!acpi_table_count()) return;            /* no ACPI: nothing to check */
    const struct acpi_sdt *m = acpi_find("APIC", 0);
    KEXPECT(m && memcmp(m->sig, "APIC", 4) == 0 && m->length >= 44);
    KEXPECT(acpi_find("APIC", 1) == NULL);
    KEXPECT(acpi_find("ZZZZ", 0) == NULL);
    const struct madt_info *mi = acpi_madt();
    KEXPECT(mi->valid && mi->cpu_count >= 1 && mi->ioapic_count >= 1);
    for (int i = 0; i < 16; i++) KEXPECT(mi->isa_gsi[i] < 256);
}

KTEST(irq_routing) {
    uint16_t masks = pic_get_masks();
    if (irq_mode() == IRQ_MODE_APIC) {
        KEXPECT(masks == 0xFFFF);
        KEXPECT(strcmp(timer_source(), "lapic") == 0 && timer_lapic_counts() > 0);
        KEXPECT(ioapic_pins() >= 16);
    } else {
        KEXPECT(strcmp(timer_source(), "pit") == 0);
        KEXPECT((masks & 1) == 0);
    }
    uint64_t t = timer_ticks();
    sleep_ms(30);
    KEXPECT(timer_ticks() >= t + 3);
}

KTEST(sync_primitives) {
    struct mutex m;
    mutex_init(&m);
    KEXPECT(mutex_trylock(&m));
    KEXPECT(!mutex_trylock(&m));
    mutex_unlock(&m);
    KEXPECT(mutex_trylock(&m));
    mutex_unlock(&m);
    struct semaphore s;
    sem_init(&s, 2);
    sem_wait(&s);
    sem_wait(&s);
    KEXPECT(s.count == 0);
    sem_post(&s);
    KEXPECT(s.count == 1);
    struct spinlock l = { 0, "t" };
    uint64_t f = spin_lock_irqsave(&l);
    KEXPECT(l.locked);
    spin_unlock_irqrestore(&l, f);
    KEXPECT(!l.locked);
}
