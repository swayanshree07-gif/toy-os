/* TernOS kernel entry point. */
#include <stdint.h>
#include "kernel.h"
#include "io.h"
#include "serial.h"
#include "kprintf.h"
#include "string.h"
#include "multiboot2.h"
#include "fb.h"
#include "console.h"
#include "gdt.h"
#include "idt.h"
#include "extable.h"
#include "pic.h"
#include "irq.h"
#include "acpi.h"
#include "timer.h"
#include "keyboard.h"
#include "mm.h"
#include "pmm.h"
#include "vmm.h"
#include "selftest.h"
#include "heap.h"
#include "sched.h"
#include "ksyms.h"
#include "ktest.h"
#include "proc.h"
#include "initrd.h"
#include "vfs.h"
#include "tty.h"

bool g_test_mode;

void qemu_exit(int code) {
    outl(0xF4, (uint32_t)code);
}

bool g_panic_backtrace_done;

void panic(const char *fmt, ...) {
    static bool panicking;
    irq_disable();
    if (panicking) {                      /* a fault while reporting a panic: give up */
        kprintf("\n*** nested panic\n");
        if (g_test_mode) qemu_exit(QEMU_EXIT_FAILURE);
        cpu_halt_forever();
    }
    panicking = true;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    kprintf("\n*** KERNEL PANIC: %s\n", buf);
    if (!g_panic_backtrace_done)
        backtrace_print(0, (uint64_t)__builtin_frame_address(0));
    if (g_test_mode) qemu_exit(QEMU_EXIT_FAILURE);
    cpu_halt_forever();
}

static bool in_long_mode(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080));
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    /* EFER.LMA (bit 10) set and paging on */
    return (lo & (1u << 10)) && (cr0 & (1ull << 31));
}

static const char *mmap_type_name(uint32_t t) {
    switch (t) {
    case 1: return "usable";
    case 2: return "reserved";
    case 3: return "acpi-reclaim";
    case 4: return "acpi-nvs";
    case 5: return "bad";
    default: return "other";
    }
}

static int test_kprintf(void) {
    char b[64];
    int fails = 0;
    ksnprintf(b, sizeof b, "%d|%5u|%-3s|%x|%08X", -42, 7u, "a", 0xbeefu, 0x1Fu);
    if (strcmp(b, "-42|    7|a  |beef|0000001F")) { kprintf("  got '%s'\n", b); fails++; }
    ksnprintf(b, sizeof b, "%lu %lx", (unsigned long)18446744073709551615ull, (unsigned long)0x123456789abcull);
    if (strcmp(b, "18446744073709551615 123456789abc")) { kprintf("  got '%s'\n", b); fails++; }
    int n = ksnprintf(b, 4, "abcdef");
    if (n != 6 || strcmp(b, "abc")) { kprintf("  truncation wrong\n"); fails++; }
    return fails;
}

/* Draw text through the console and verify it pixel-by-pixel, including a scroll. */
static int test_console(void) {
    uint32_t cols, rows, c, r;
    con_get_size(&cols, &rows);
    con_get_cursor(&c, &r);
    int fails = 0;

    /* 1. A line we just printed is visible where the cursor says. */
    con_putc('\n');
    con_get_cursor(&c, &r);
    const char *probe = "Tern 0123 {ok}";
    for (const char *p = probe; *p; p++) con_putc(*p);
    for (uint32_t i = 0; probe[i]; i++)
        if (!con_cell_matches(i, r, probe[i])) { kprintf("  cell %u mismatch\n", i); fails++; }

    /* 2. Fill the screen so it scrolls; line k is "L<k>". Afterwards the last line
     *    printed sits on row rows-2 (cursor is on the empty row below it). */
    char line[16];
    uint32_t n = rows + 3;
    for (uint32_t k = 0; k < n; k++) {
        ksnprintf(line, sizeof line, "\nL%u", k);
        for (char *p = line; *p; p++) con_putc(*p);
    }
    con_putc('\n');
    con_get_cursor(&c, &r);
    if (r != rows - 1 || c != 0) { kprintf("  cursor %u,%u after scroll\n", c, r); fails++; }
    ksnprintf(line, sizeof line, "L%u", n - 1);
    for (uint32_t i = 0; line[i]; i++)
        if (!con_cell_matches(i, rows - 2, line[i])) { kprintf("  scrolled cell %u mismatch\n", i); fails++; }
    ksnprintf(line, sizeof line, "L%u", n - rows + 1);
    for (uint32_t i = 0; line[i]; i++)
        if (!con_cell_matches(i, 0, line[i])) { kprintf("  top cell %u mismatch\n", i); fails++; }
    return fails;
}

/* ---- Day 4: exceptions ---- */
static volatile int trap_hits;
static volatile uint64_t trap_rip;

static void int3_handler(struct regs *r) {
    trap_hits++;
    trap_rip = r->rip;          /* int3 is a trap: rip already points past it */
}

static void ud_handler(struct regs *r) {
    trap_hits++;
    trap_rip = r->rip;
    r->rip += 2;                /* skip the 2-byte ud2 so the test can continue */
}

static volatile int de_hits;
static void de_handler(struct regs *r) {
    de_hits++;
    r->rax = 0x7E57;            /* divq below is 3 bytes: fake a quotient and skip it */
    r->rip += 3;
}

static void test_exceptions(void) {
    isr_handler_t old = isr_set_handler(3, int3_handler);
    uint64_t after;
    trap_hits = 0;
    __asm__ volatile("int3; lea 0(%%rip), %0" : "=r"(after));
    /* lea's rip-relative value is the address after lea (7 bytes long), so the
     * address right after int3 is after - 7. */
    if (trap_hits != 1 || trap_rip != after - 7)
        panic("int3: hits=%d rip=%lx expected %lx", trap_hits, trap_rip, after - 7);
    isr_set_handler(3, old);
    kprintf("[TEST] idt: int3 caught at %lx\n", trap_rip);

    old = isr_set_handler(6, ud_handler);
    trap_hits = 0;
    uint64_t at;
    __asm__ volatile("lea 0(%%rip), %0; ud2" : "=r"(at));
    if (trap_hits != 1 || trap_rip != at) panic("ud2: hits=%d rip=%lx expected %lx", trap_hits, trap_rip, at);
    isr_set_handler(6, old);
    kprintf("[TEST] idt: ud2 caught at %lx, resumed\n", trap_rip);

    old = isr_set_handler(0, de_handler);
    uint64_t q, zero = 0;
    /* force the encoding "divq %rcx" (48 f7 f1, 3 bytes) */
    __asm__ volatile("xorl %%edx, %%edx; movq $5, %%rax; divq %%rcx"
                     : "=a"(q) : "c"(zero) : "rdx");
    if (de_hits != 1 || q != 0x7E57) panic("#DE: hits=%d q=%lx", de_hits, q);
    isr_set_handler(0, old);
    kprintf("[TEST] idt: divide error caught\n");

    /* Page fault and #GP recovered through the exception table. */
    uint64_t v = 0;
    long rc = probe_read64((const void *)0x7FFF00000000ull, &v);   /* unmapped (>4 GiB) */
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    if (rc != -1 || cr2 != 0x7FFF00000000ull) panic("#PF probe: rc=%ld cr2=%lx", rc, cr2);
    kprintf("[TEST] idt: page fault fixed up (cr2=%lx)\n", cr2);
    rc = probe_read64((const void *)0x8000000000000000ull, &v);    /* non-canonical */
    if (rc != -1) panic("#GP probe: rc=%ld", rc);
    static const uint64_t magic = 0x5445524E;   /* "TERN" */
    rc = probe_read64(&magic, &v);
    if (rc != 0 || v != magic) panic("probe of valid address: rc=%ld v=%lx", rc, v);
    kprintf("[TEST] idt: #GP fixed up, valid probe ok\n");
}

/* ---- Day 5: PIC + PIT ---- */
static void test_timer(void) {
    /* Independent reference: TSC rate measured with polled PIT channel 2. */
    uint64_t t0 = rdtsc();
    pit_busy_wait_us(50000);
    uint64_t tsc_per_ms = (rdtsc() - t0) / 50;
    kprintf("tsc: ~%lu kHz\n", tsc_per_ms);

    uint64_t start = timer_ticks();
    while (timer_ticks() == start) __asm__ volatile("hlt");   /* align to a tick edge */
    start = timer_ticks();
    t0 = rdtsc();
    while (timer_ticks() < start + 50) __asm__ volatile("hlt");
    uint64_t ms = (rdtsc() - t0) / (tsc_per_ms ? tsc_per_ms : 1);
    if (ms < 400 || ms > 600) panic("timer: 50 ticks took %lu ms by TSC (expected ~500)", ms);
    kprintf("[TEST] timer: 50 ticks in %lu ms\n", ms);

    uint64_t before = timer_ms();
    sleep_ms(120);
    uint64_t slept = timer_ms() - before;
    if (slept < 120 || slept > 140) panic("sleep_ms(120) took %lu ms", slept);
    kprintf("[TEST] timer: sleep_ms(120) -> %lu ms\n", slept);
}

/* ---- Day 13: ACPI MADT, APIC vs PIC mode ---- */
static void test_irq_mode(void) {
    const struct madt_info *m = acpi_madt();
    if (acpi_table_count()) {
        if (!m->valid) panic("acpi: no MADT");
        kprintf("[TEST] acpi: %u tables, madt: %u cpu(s), %u ioapic(s), lapic %lx, "
                "%u isa overrides, irq0->gsi%u\n", acpi_table_count(), m->cpu_count,
                m->ioapic_count, m->lapic_phys, m->overrides, m->isa_gsi[0]);
    }
    uint16_t masks = pic_get_masks();
    if (irq_mode() == IRQ_MODE_APIC) {
        if (masks != 0xFFFF) panic("apic mode but 8259 masks are %x", masks);
        uint32_t c = timer_lapic_counts();
        if (strcmp(timer_source(), "lapic") || !c) panic("apic mode without lapic timer");
        kprintf("[TEST] apic: lapic id %u ver %x, ioapic %u pins, 8259 fully masked\n",
                lapic_id(), lapic_version(), ioapic_pins());
        kprintf("[TEST] timer source: lapic (%u counts/tick at div 16)\n", c);
    } else {
        if (strcmp(timer_source(), "pit")) panic("pic mode but timer is %s", timer_source());
        if (masks & 1) panic("pic mode but IRQ0 masked (%x)", masks);
        kprintf("[TEST] timer source: pit (8259 masks %x)\n", masks);
    }
}

/* ---- crash test: unbounded recursion must hit the thread's stack guard page ---- */
static volatile uint64_t recurse_limit = ~0ull;   /* volatile: opaque to the compiler */
static uint64_t recurse(uint64_t n) {
    volatile char pad[256];
    pad[0] = (char)n;
    if (n >= recurse_limit) return 0;
    return recurse(n + 1) + pad[0];
}
static void recurse_thread(void *arg) { (void)arg; recurse(0); }

/* ---- crash test: a panic two calls deep must print a symbolised backtrace ---- */
__attribute__((noinline)) static void panic_level2(int x) {
    panic("crash test: deliberate panic (x=%d)", x);
}
__attribute__((noinline)) static void panic_level1(int x) {
    panic_level2(x + 1);
    __asm__ volatile("");
}

/* ---- Day 6: keyboard (the harness types "hi T!<enter>" via QEMU sendkey) ---- */
static void test_keyboard(void) {
    char line[32];
    unsigned n = 0;
    kprintf("[TEST] kbd: waiting for input\n");
    for (;;) {
        int c = kbd_getc_timeout(15000);
        if (c < 0) { line[n] = 0; panic("kbd: timeout, got '%s'", line); }
        if (c == '\n' || n == sizeof line - 1) break;
        line[n++] = (char)c;
    }
    line[n] = 0;
    kprintf("[TEST] kbd: got '%s'\n", line);
}

/* Normal boot: run the shell on the console, restarting it whenever it exits. */
static __attribute__((noreturn)) void run_shell_forever(void) {
    for (;;) {
        int err = 0;
        const char *argv[] = { "tsh", NULL };
        kprintf("starting /bin/tsh\n");
        struct process *p = proc_spawn_elf("/bin/tsh", argv, &err);
        if (!p) {
            kprintf("cannot start /bin/tsh (%d); idle.\n", err);
            for (;;) __asm__ volatile("hlt");
        }
        int code = proc_wait(p, NULL);
        kprintf("shell exited with %d, restarting\n", code);
    }
}

void kmain(uint32_t magic, uint32_t info_addr) {
    serial_init();

    struct boot_info bi;
    int rc = mb2_parse(magic, info_addr, &bi);
    if (rc == 0 && fb_init(bi.fb)) con_init();
    kprintf("\n" TERNOS_NAME " " TERNOS_VERSION " booting\n");
    if (rc) panic("multiboot2 info invalid (rc=%d, magic=%x)", rc, magic);

    if (!bi.cmdline) bi.cmdline = "";
    g_test_mode = strstr(bi.cmdline, "test") != NULL;

    kassert(in_long_mode());
    kprintf("[TEST] boot: long mode ok\n");

    kprintf("bootloader: %s\n", bi.bootloader ? bi.bootloader : "?");
    kprintf("cmdline: '%s'\n", bi.cmdline ? bi.cmdline : "");
    kprintf("memory map (%u entries):\n", bi.mmap_count);
    for (uint32_t i = 0; i < bi.mmap_count; i++) {
        struct mb2_mmap_entry *e =
            (struct mb2_mmap_entry *)((uintptr_t)bi.mmap + i * bi.mmap_entry_size);
        kprintf("  %016lx-%016lx %s\n", e->addr, e->addr + e->len - 1, mmap_type_name(e->type));
    }
    kprintf("[TEST] mb2: %u tags, mmap %u entries, %lu MiB usable\n",
            bi.tag_count, bi.mmap_count, bi.usable_bytes >> 20);
    if (bi.fb)
        kprintf("[TEST] mb2: framebuffer %ux%ux%u at %lx pitch %u\n",
                bi.fb->width, bi.fb->height, bi.fb->bpp, bi.fb->addr, bi.fb->pitch);
    else
        kprintf("mb2: no framebuffer tag\n");

    gdt_init();
    {
        uint16_t cs, ds, ss, tr;
        __asm__ volatile("mov %%cs, %0; mov %%ds, %1; mov %%ss, %2; str %3"
                         : "=r"(cs), "=r"(ds), "=r"(ss), "=r"(tr));
        if (cs != GDT_KCODE || ds != GDT_KDATA || ss != GDT_KDATA || tr != GDT_TSS)
            panic("gdt: cs=%x ds=%x ss=%x tr=%x", cs, ds, ss, tr);
        kprintf("[TEST] gdt ok (cs=%x ds=%x tr=%x)\n", cs, ds, tr);
    }

    idt_init();
    kprintf("idt loaded (256 vectors)\n");
    if (g_test_mode) test_exceptions();
    /* Negative tests (separate boots, see tests/run_tests.sh): these must panic. */
    if (strstr(bi.cmdline, "crash=pf")) {
        kprintf("crash test: unhandled page fault\n");
        *(volatile uint64_t *)0x7FFF00000000ull = 1;
    }
    if (strstr(bi.cmdline, "crash=df")) {
        /* #PF with an unusable stack -> #DF, which runs on the IST1 stack */
        kprintf("crash test: double fault\n");
        __asm__ volatile("movq $0x7FFF00001000, %rsp; pushq $0");
    }

    pmm_init(&bi);
    vmm_init(&bi);
    heap_init();
    if (g_test_mode) { test_pmm(); test_vmm(); test_heap(); }
    initrd_init(&bi);
    if (g_test_mode) test_initrd();
    if (strstr(bi.cmdline, "crash=kfree")) {
        kprintf("crash test: double kfree\n");
        void *p = kmalloc(32);
        kfree(p);
        kfree(p);
    }
    if (strstr(bi.cmdline, "crash=null")) {
        /* NULL-pointer read: the lower half is unmapped once vmm_init() has run */
        kprintf("crash test: null pointer read\n");
        volatile uint64_t *volatile p = (volatile uint64_t *)0x18;
        (void)*p;
    }

    irq_init(&bi, !strstr(bi.cmdline, "noapic"));
    timer_init(TIMER_HZ);
    irq_enable();
    serial_enable_rx_irq();
    kprintf("interrupts on: %s mode, tick %u Hz from %s\n",
            irq_mode() == IRQ_MODE_APIC ? "apic" : "pic", TIMER_HZ, timer_source());
    if (g_test_mode) { test_irq_mode(); test_timer(); }

    sched_init();
    kprintf("scheduler: main thread adopted, idle thread created\n");
    if (g_test_mode) test_sched();
    sched_set_preempt(true);
    vfs_init();
    devfs_init();
    {
        int n = vfs_populate_from_initrd();
        struct vfs_stats vs;
        vfs_get_stats(&vs);
        kprintf("vfs: ramfs root, %d initrd files copied, %lu nodes\n", n, vs.nodes);
        if (g_test_mode) {
            const struct initrd *rd = initrd_boot();
            if (!rd || n != (int)rd->count) panic("vfs: copied %d of %u initrd files", n, rd ? rd->count : 0);
            test_vfs();
        }
    }
    syscall_init();
    proc_init();
    tty_init();
    if (g_test_mode) { test_user(); test_elf(); test_wait(); test_kill(); test_fpu(); }
    if (strstr(bi.cmdline, "crash=stackovf")) {
        kprintf("crash test: kernel stack overflow\n");
        struct thread *t = thread_create("deep-recursion", recurse_thread, NULL);
        kassert(t);
        thread_join(t);
    }

    if (g_test_mode) {
        int nf = ktest_run_all();
        if (nf) panic("selftest: %d unit test(s) failed", nf);
    }
    if (strstr(bi.cmdline, "crash=panic")) panic_level1(41);

    bool have_kbd = kbd_init();
    kprintf("keyboard: %s\n", have_kbd ? "ps/2 ready" : "no 8042 controller");
    if (g_test_mode && strstr(bi.cmdline, "kbd")) { test_keyboard(); test_tty(); }

    int f = test_kprintf();
    if (f) panic("kprintf self-test: %d failures", f);
    kprintf("[TEST] kprintf ok\n");

    if (con_active()) {
        uint32_t cols, rows;
        con_get_size(&cols, &rows);
        kprintf("fb console: %ux%u cells\n", cols, rows);
        if (g_test_mode) {
            f = test_console();
            if (f) panic("console self-test: %d failures", f);
            kprintf("[TEST] fb: console ok\n");
        }
    } else {
        kprintf("fb console unavailable\n");
    }

    if (g_test_mode) test_shell(strstr(bi.cmdline, "kbd") != NULL);

    kprintf("[TEST] all done\n");
    if (g_test_mode) qemu_exit(QEMU_EXIT_SUCCESS);
    run_shell_forever();
}
