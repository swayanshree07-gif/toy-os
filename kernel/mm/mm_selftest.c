/* Self-tests for the physical and virtual memory managers (run with "test" cmdline). */
#include "selftest.h"
#include "pmm.h"
#include "vmm.h"
#include "mm.h"
#include "idt.h"
#include "extable.h"
#include "kernel.h"
#include "kprintf.h"

#define TAG 0x7E4E7E4E00000000ull

static void check_not_reserved(uint64_t pa) {
    const struct pmm_range *res;
    unsigned n = pmm_reserved(&res);
    if (!pa || (pa & (PAGE_SIZE - 1))) panic("pmm: bad frame %lx", pa);
    for (unsigned i = 0; i < n; i++)
        if (pa < res[i].end && res[i].start < pa + PAGE_SIZE)
            panic("pmm: frame %lx overlaps reserved %s", pa, res[i].what);
}

void test_pmm(void) {
    uint64_t free0 = pmm_free_frames();
    static uint64_t frames[256];

    /* 1. Distinct, writable frames: tag each, then verify no tag was overwritten. */
    for (unsigned i = 0; i < 256; i++) {
        uint64_t pa = pmm_alloc();
        check_not_reserved(pa);
        if (!pmm_is_used(pa)) panic("pmm: %lx not marked used", pa);
        uint64_t *p = phys_to_virt(pa);
        p[0] = pa ^ TAG;
        p[511] = ~pa;
        frames[i] = pa;
    }
    for (unsigned i = 0; i < 256; i++) {
        uint64_t *p = phys_to_virt(frames[i]);
        if (p[0] != (frames[i] ^ TAG) || p[511] != ~frames[i])
            panic("pmm: frame %lx handed out twice", frames[i]);
    }
    if (pmm_free_frames() != free0 - 256) panic("pmm: free count off after alloc");
    for (unsigned i = 0; i < 256; i += 2) pmm_free(frames[i]);        /* free in two passes */
    for (unsigned i = 1; i < 256; i += 2) pmm_free(frames[i]);
    if (pmm_free_frames() != free0) panic("pmm: free count off after free");

    /* 2. Exhaust all memory, chaining frames through their first word, then free all. */
    uint64_t head = 0, n = 0, pa;
    while ((pa = pmm_alloc())) {
        check_not_reserved(pa);
        *(uint64_t *)phys_to_virt(pa) = head;
        head = pa;
        n++;
    }
    if (n != free0 || pmm_free_frames() != 0)
        panic("pmm: exhausted after %lu frames, expected %lu", n, free0);
    while (head) {
        uint64_t next = *(uint64_t *)phys_to_virt(head);
        pmm_free(head);
        head = next;
    }
    if (pmm_free_frames() != free0) panic("pmm: free count %lu != %lu after refill",
                                          pmm_free_frames(), free0);
    kprintf("[TEST] pmm ok: %lu frames free (%lu MiB), exhausted and refilled\n",
            free0, free0 >> 8);
}

static uint64_t cr2(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}

static volatile int nx_faults;
static volatile uint64_t nx_target;

/* Instruction-fetch page fault on nx_target: return to the caller as if it had run `ret`. */
static void nx_pf_handler(struct regs *r) {
    if ((r->error & 16) && r->rip == nx_target && (r->cs & 3) == 0) {
        nx_faults++;
        r->rip = *(uint64_t *)r->rsp;
        r->rsp += 8;
        return;
    }
    regs_dump(r);
    panic("unexpected page fault during NX test");
}

static const uint64_t ro_constant = 0x524F;   /* lives in .rodata */

void test_vmm(void) {
    uint64_t k = vmm_kernel_pml4(), pa, fl;
    if ((read_cr3() & PTE_ADDR) != k) panic("vmm: cr3 %lx is not the kernel PML4", read_cr3());

    /* Kernel text: higher half, translates to its load address, read-only + executable. */
    uint64_t text = (uint64_t)test_vmm;
    if (text < KERNEL_VMA) panic("vmm: kernel not in the higher half (%lx)", text);
    if (!vmm_translate(k, text, &pa, &fl) || pa != virt_to_phys(test_vmm))
        panic("vmm: translate(text) wrong");
    if ((fl & PTE_W) || (fl & PTE_NX)) panic("vmm: text flags %lx", fl);
    /* Direct map uses 2 MiB pages and covers the framebuffer area (first 4 GiB). */
    if (!vmm_translate(k, HHDM_BASE + 0x123456, &pa, &fl) || pa != 0x123456 || !(fl & PTE_PS))
        panic("vmm: translate(direct map) wrong");
    if (!vmm_translate(k, HHDM_BASE + 0xFFFFF000ull, &pa, NULL)) panic("vmm: 4 GiB not mapped");
    kprintf("[TEST] vmm: kernel at %lx (phys %lx), direct map ok\n", text,
            virt_to_phys(test_vmm));

    /* Identity map is gone: the kernel's own physical address must fault now. */
    uint64_t v;
    uint64_t low = virt_to_phys(&ro_constant);
    if (probe_read64((void *)low, &v) != -1 || cr2() != low)
        panic("vmm: identity mapping still present at %lx", low);

    /* map / alias / translate / unmap */
    uint64_t frame = pmm_alloc_zeroed();
    uint64_t va = KMAP_BASE + 7 * PAGE_SIZE, va2 = KMAP_BASE + (2ull << 20) + 3 * PAGE_SIZE;
    if (vmm_map(k, va, frame, VMM_KERNEL_RW)) panic("vmm: map failed");
    if (vmm_map(k, va, frame, VMM_KERNEL_RW) != -1) panic("vmm: double map not refused");
    *(volatile uint64_t *)(va + 8) = 0x7E4ED00Dull;
    if (*(uint64_t *)((char *)phys_to_virt(frame) + 8) != 0x7E4ED00Dull)
        panic("vmm: write through mapping not visible in the direct map");
    if (vmm_map(k, va2, frame, VMM_KERNEL_RW)) panic("vmm: alias map failed");
    if (*(volatile uint64_t *)(va2 + 8) != 0x7E4ED00Dull) panic("vmm: alias mismatch");
    if (!vmm_translate(k, va + 0x10, &pa, NULL) || pa != frame + 0x10)
        panic("vmm: translate(va) wrong");
    uint64_t old = 0;
    if (!vmm_unmap(k, va, &old) || old != frame) panic("vmm: unmap failed");
    if (vmm_unmap(k, va, NULL)) panic("vmm: second unmap succeeded");
    if (probe_read64((void *)va, &v) != -1 || cr2() != va) panic("vmm: unmapped page readable");
    if (vmm_translate(k, va, NULL, NULL)) panic("vmm: unmapped page translates");
    if (*(volatile uint64_t *)(va2 + 8) != 0x7E4ED00Dull) panic("vmm: alias lost");
    vmm_unmap(k, va2, NULL);
    kprintf("[TEST] vmm ok: map/alias/translate/unmap, identity map gone (cr2=%lx)\n", va);

    /* W^X: kernel text and rodata are read-only (CR0.WP). */
    if (probe_write64((void *)text, 0) != -1) panic("vmm: kernel text writable");
    if (probe_write64((void *)&ro_constant, 0) != -1) panic("vmm: rodata writable");

    /* NX: a `ret` in an NX page must fault on fetch; mapped executable it must run. */
    *(volatile uint8_t *)phys_to_virt(frame) = 0xC3;           /* ret */
    uint64_t code = KMAP_BASE + 0x40 * PAGE_SIZE;
    nx_target = code;
    isr_handler_t prev = isr_set_handler(14, nx_pf_handler);
    vmm_map(k, code, frame, VMM_KERNEL_RW);                    /* RW + NX */
    ((void (*)(void))code)();
    if (nx_faults != 1) panic("vmm: NX page executed (faults=%d)", nx_faults);
    vmm_unmap(k, code, NULL);
    vmm_map(k, code, frame, VMM_KERNEL_RX);                    /* read + execute */
    ((void (*)(void))code)();
    isr_set_handler(14, prev);
    if (nx_faults != 1) panic("vmm: executable page faulted");
    vmm_unmap(k, code, NULL);
    pmm_free(frame);
    kprintf("[TEST] vmm: W^X and NX enforced\n");
}
