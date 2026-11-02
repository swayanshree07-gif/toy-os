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

bool g_test_mode;

void qemu_exit(int code) {
    outl(0xF4, (uint32_t)code);
}

void panic(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    kprintf("\n*** KERNEL PANIC: %s\n", buf);
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

void kmain(uint32_t magic, uint32_t info_addr) {
    serial_init();

    struct boot_info bi;
    int rc = mb2_parse(magic, info_addr, &bi);
    if (rc == 0 && fb_init(bi.fb)) con_init();
    kprintf("\n" TERNOS_NAME " " TERNOS_VERSION " booting\n");
    if (rc) panic("multiboot2 info invalid (rc=%d, magic=%x)", rc, magic);

    g_test_mode = bi.cmdline && strstr(bi.cmdline, "test");

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

    kprintf("[TEST] all done\n");
    if (g_test_mode) qemu_exit(QEMU_EXIT_SUCCESS);
    kprintf("idle.\n");
    cpu_halt_forever();
}
