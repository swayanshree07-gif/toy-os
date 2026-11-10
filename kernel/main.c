/* TernOS kernel entry point. */
#include <stdint.h>
#include "entry.h"
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
#include "pci.h"
#include "ahci.h"
#include "syscall_nr.h"
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

/* ---- Day 13 session: PCI enumeration (q35: host bridge 8086:29c0, AHCI at 00:1f.2) ---- */

/* AHCI: the test boots attach build/testdisk.img (64 sectors, tools/mktestdisk.py) as a
 * second SATA disk; every sector carries its LBA and a known byte pattern. */
static bool check_sector(const uint8_t *s, unsigned lba) {
    char head[32];
    ksnprintf(head, sizeof head, "TERNDISK LBA %08u\n", lba);
    size_t hl = strlen(head);
    if (memcmp(s, head, hl)) return false;
    for (unsigned i = hl; i < 512; i++) if (s[i] != (uint8_t)(lba * 7 + i)) return false;
    return true;
}

/* FAT16: the test boots attach build/fatdisk.img (tools/mkfatdisk.py), mounted at /disk. */
static uint8_t big_byte(uint32_t i) { return (uint8_t)(i * 31 + i / 256); }
static void fat_expect(const char *path, const char *want) {
    struct file *f;
    char buf[128];
    int rc = vfs_open(NULL, path, O_RDONLY, &f);
    if (rc) panic("fat: open %s: %d", path, rc);
    long n = file_read(f, buf, sizeof buf - 1);
    file_close(f);
    size_t wl = strlen(want);
    if (n != (long)wl || memcmp(buf, want, wl)) panic("fat: %s read %ld bytes, wrong content", path, n);
}
/* Day 16: writing. Everything here goes to the disk at once (write-through). */
static uint8_t wpat(uint32_t i) { return (uint8_t)(i * 7 + 3); }
static void fat_write_file(const char *path, int flags, const void *data, size_t len) {
    struct file *f;
    int rc = vfs_open(NULL, path, O_WRONLY | O_CREAT | flags, &f);
    if (rc) panic("fat: open %s for writing: %d", path, rc);
    long n = file_write(f, data, len);
    file_close(f);
    if (n != (long)len) panic("fat: write %s: %ld", path, n);
}
#define PERSIST_TEXT "written by TernOS boot 1, read back by boot 2\n"
static void test_fat_write(void) {
    struct fat_stats st0, st1;
    int err = 0;
    /* persistence across boots (tests/run_tests.sh boots twice on one writable copy) */
    struct vnode *pv = vfs_lookup(NULL, "/disk/persist.txt", &err);
    if (pv) {
        vfs_put(pv);
        fat_expect("/disk/persist.txt", PERSIST_TEXT);
        fat_expect("/disk/pdir/p2.txt", "second file in a new directory\n");
        fat_expect("/disk/pdir/Long Persisted Name.txt", "renamed long name\n");
        fat_expect("/disk/pdir/Moved Dir 2/Note In Moved Dir.md", "note\n");
        if (vfs_unlink(NULL, "/disk/pdir/Long Persisted Name.txt") ||
            vfs_unlink(NULL, "/disk/pdir/Moved Dir 2/Note In Moved Dir.md") ||
            vfs_unlink(NULL, "/disk/pdir/Moved Dir 2") ||
            vfs_unlink(NULL, "/disk/pdir/p2.txt") || vfs_unlink(NULL, "/disk/pdir") ||
            vfs_unlink(NULL, "/disk/persist.txt"))
            panic("fat: removing the persisted files");
        kprintf("[TEST] fat: files written by the previous boot read back and removed\n");
    } else {
        fat_write_file("/disk/persist.txt", 0, PERSIST_TEXT, sizeof PERSIST_TEXT - 1);
        if (vfs_mkdir(NULL, "/disk/pdir")) panic("fat: mkdir pdir");
        fat_write_file("/disk/pdir/p2.txt", 0, "second file in a new directory\n", 31);
        /* Day 24: long names + rename (a file across directories, a whole directory) */
        fat_write_file("/disk/tmp1.txt", 0, "renamed long name\n", 18);
        if (vfs_rename(NULL, "/disk/tmp1.txt", "/disk/pdir/Long Persisted Name.txt"))
            panic("fat: rename to long name");
        if (vfs_mkdir(NULL, "/disk/Moved Dir")) panic("fat: mkdir long name");
        fat_write_file("/disk/Moved Dir/Note In Moved Dir.md", 0, "note\n", 5);
        if (vfs_rename(NULL, "/disk/Moved Dir", "/disk/pdir/Moved Dir 2")) panic("fat: rename dir");
        kprintf("[TEST] fat: persist.txt + pdir/p2.txt written for the next boot\n");
    }
    fat_get_stats(&st0);
    /* big file written in odd chunks (crosses sector/cluster boundaries), read back */
    static uint8_t buf[3000];
    struct file *f;
    if (vfs_open(NULL, "/disk/w1.dat", O_RDWR | O_CREAT, &f)) panic("fat: create w1.dat");
    uint32_t pos = 0, chunks[] = { 1, 511, 2, 700, 513, 1000, 37 };
    for (int k = 0; pos < 20000; k++) {
        uint32_t n = chunks[k % 7];
        if (n > 20000 - pos) n = 20000 - pos;
        for (uint32_t i = 0; i < n; i++) buf[i] = wpat(pos + i);
        if (file_write(f, buf, n) != (long)n) panic("fat: w1.dat write at %u", pos);
        pos += n;
    }
    /* overwrite in the middle, then read everything back through the same fd */
    if (file_seek(f, 5000, SEEK_SET) != 5000 || file_write(f, "MIDDLE", 6) != 6) panic("fat: overwrite");
    file_seek(f, 0, SEEK_SET);
    for (pos = 0; pos < 20000;) {
        long n = file_read(f, buf, 3000);
        if (n <= 0) panic("fat: w1.dat read at %u: %ld", pos, n);
        for (long i = 0; i < n; i++) {
            uint32_t p = pos + (uint32_t)i;
            uint8_t want = p >= 5000 && p < 5006 ? (uint8_t)"MIDDLE"[p - 5000] : wpat(p);
            if (buf[i] != want) panic("fat: w1.dat byte %u wrong", p);
        }
        pos += (uint32_t)n;
    }
    /* write past the end: the hole reads as zeros */
    if (file_seek(f, 21000, SEEK_SET) != 21000 || file_write(f, "END", 3) != 3) panic("fat: hole write");
    file_seek(f, 19990, SEEK_SET);
    if (file_read(f, buf, 2000) != 1013) panic("fat: read over the hole");
    for (int i = 10; i < 1010; i++) if (buf[i]) panic("fat: hole byte %d not zero", i);
    if (memcmp(buf + 1010, "END", 3)) panic("fat: data after hole");
    file_close(f);
    /* append and truncate */
    fat_write_file("/disk/hello2.txt", 0, "one\n", 4);
    fat_write_file("/disk/hello2.txt", O_APPEND, "two\n", 4);
    fat_expect("/disk/hello2.txt", "one\ntwo\n");
    fat_write_file("/disk/hello2.txt", O_TRUNC, "three\n", 6);
    fat_expect("/disk/hello2.txt", "three\n");
    /* a new directory with enough files to need more than one directory cluster */
    if (vfs_mkdir(NULL, "/disk/newdir")) panic("fat: mkdir newdir");
    char name[40];
    for (int i = 0; i < 40; i++) {
        ksnprintf(name, sizeof name, i % 2 ? "/disk/newdir/f%d.txt" : "/disk/newdir/File number %d.text", i);
        fat_write_file(name, 0, name, strlen(name));
    }
    for (int i = 0; i < 40; i++) {
        ksnprintf(name, sizeof name, i % 2 ? "/disk/newdir/f%d.txt" : "/disk/newdir/File number %d.text", i);
        fat_expect(name, name);
    }
    /* Day 24: long names and rename within the disk */
    if (vfs_mkdir(NULL, "/disk/a.b.c") || vfs_unlink(NULL, "/disk/a.b.c")) panic("fat: long dir name");
    if (vfs_open(NULL, "/disk/a name that is far too long for the vfs: 56 bytes!!!!!!!", O_WRONLY | O_CREAT, &f) != -ENAMETOOLONG)
        panic("fat: 56-byte name accepted");
    if (vfs_rename(NULL, "/disk/hello2.txt", "/disk/newdir/Hello Two.txt")) panic("fat: rename into dir");
    fat_expect("/disk/newdir/Hello Two.txt", "three\n");
    if (vfs_rename(NULL, "/disk/newdir/f1.txt", "/disk/newdir/f3.txt")) panic("fat: rename over file");
    fat_expect("/disk/newdir/f3.txt", "/disk/newdir/f1.txt");
    if (vfs_rename(NULL, "/disk/newdir/Hello Two.txt", "/disk/hello2.txt")) panic("fat: rename back");
    if (vfs_rename(NULL, "/disk/hello2.txt", "/tmp-not-disk") != -EXDEV) panic("fat: rename to RAM not EXDEV");
    if (vfs_unlink(NULL, "/disk/newdir") != -ENOTEMPTY) panic("fat: rmdir non-empty");
    /* remove everything again: all clusters must come back */
    for (int i = 0; i < 40; i++) {
        if (i == 1) continue;                               /* renamed over f3 */
        ksnprintf(name, sizeof name, i % 2 ? "/disk/newdir/f%d.txt" : "/disk/newdir/File number %d.text", i);
        if (vfs_unlink(NULL, name)) panic("fat: unlink %s", name);
    }
    if (vfs_unlink(NULL, "/disk/newdir") || vfs_unlink(NULL, "/disk/w1.dat") ||
        vfs_unlink(NULL, "/disk/hello2.txt"))
        panic("fat: unlink");
    if (vfs_lookup(NULL, "/disk/w1.dat", &err)) panic("fat: w1.dat still there");
    fat_get_stats(&st1);
    if (st1.free_clusters != st0.free_clusters)
        panic("fat: cluster leak: %lu free before, %lu after", st0.free_clusters, st1.free_clusters);
    fat_expect("/disk/hello.txt", "Hello from a FAT16 disk!\n");
    kprintf("[TEST] fat write: 20000-byte file in odd chunks + overwrite + hole, append, truncate, "
            "mkdir with 40 files (dir grows, long names), rename/EXDEV/ENOTEMPTY errors, all unlinked, "
            "%lu clusters free (no leak), %lu sector writes\n", st1.free_clusters, st1.sector_writes);
}

static void test_fat(void) {
    int lerr = 0;
    struct vnode *v = vfs_lookup(NULL, "/disk/big.bin", &lerr);
    if (!v) panic("fat: /disk/big.bin missing (is fatdisk.img attached?)");
    struct tern_stat st;
    vfs_stat(v, &st);
    if (st.type != TERN_FT_FILE || st.size != 100000) panic("fat: big.bin type %u size %lu", st.type, st.size);
    struct file *f = vfs_open_node(v, O_RDONLY);
    vfs_put(v);
    if (!f) panic("fat: open big.bin");
    /* odd chunk sizes so reads straddle sector and cluster boundaries */
    static uint8_t buf[1000];
    uint32_t pos = 0, chunks[] = { 1, 511, 2, 700, 513, 1000, 37 };
    for (int k = 0; pos < 100000; k++) {
        long n = file_read(f, buf, chunks[k % 7]);
        if (n <= 0) panic("fat: big.bin read at %u returned %ld", pos, n);
        for (long i = 0; i < n; i++)
            if (buf[i] != big_byte(pos + (uint32_t)i)) panic("fat: big.bin byte %u wrong", pos + (uint32_t)i);
        pos += (uint32_t)n;
    }
    if (pos != 100000 || file_read(f, buf, 10) != 0) panic("fat: big.bin length/EOF wrong (%u)", pos);
    if (file_seek(f, 77777, SEEK_SET) != 77777 || file_read(f, buf, 300) != 300) panic("fat: seek+read");
    for (int i = 0; i < 300; i++) if (buf[i] != big_byte(77777u + (uint32_t)i)) panic("fat: byte after seek");
    if (file_write(f, "x", 1) != -EBADF) panic("fat: write on read-only fd");
    file_close(f);
    fat_expect("/disk/hello.txt", "Hello from a FAT16 disk!\n");
    fat_expect("/disk/docs/deep/nest.txt", "nested ok\n");
    fat_expect("/disk/empty.txt", "");
    fat_expect("/disk/docs/readme.txt", "TernOS FAT16 test disk.\nThis file lives in /disk/docs.\nThird line.\n");
    fat_expect("/disk/hello.txt", "Hello from a FAT16 disk!\n");
    int err = 0;
    if (vfs_lookup(NULL, "/disk/eleted.txt", &err) || vfs_lookup(NULL, "/disk/terndisk", &err))
        panic("fat: deleted entry or volume label visible");
    struct fat_stats fs;
    fat_get_stats(&fs);
    kprintf("[TEST] fat: /disk big.bin 100000 bytes (scattered clusters) verified in odd chunks + seek, "
            "nested dirs, empty file (%lu sector reads, %lu cache hits)\n",
            fs.sector_reads, fs.cache_hits);
    test_fat_write();
}

static void test_ahci(void) {
    int td = -1;
    for (int i = 0; i < ahci_disk_count(); i++) if (ahci_disk_get(i)->sectors == 64) td = i;
    if (td < 0) panic("ahci: test disk (64 sectors) not found, %d disks", ahci_disk_count());
    const struct ahci_disk *d = ahci_disk_get(td);
    uint8_t *buf = kmalloc(8 * 512);
    if (!buf) panic("ahci: kmalloc");
    static const unsigned single[] = {0, 37, 63};
    for (unsigned k = 0; k < 3; k++) {
        memset(buf, 0xAA, 512);
        int r = ahci_read(td, single[k], 1, buf);
        if (r || !check_sector(buf, single[k])) panic("ahci: lba %u read %d / bad data", single[k], r);
    }
    memset(buf, 0, 8 * 512);
    if (ahci_read(td, 10, 8, buf)) panic("ahci: 8-sector read failed");
    for (unsigned k = 0; k < 8; k++)
        if (!check_sector(buf + k * 512, 10 + k)) panic("ahci: 8-sector read, sector %u bad", 10 + k);
    if (ahci_read(td, 60, 8, buf) != -EINVAL || ahci_read(td, 0, 0, buf) != -EINVAL ||
        ahci_read(td, 0, 9, buf) != -EINVAL || ahci_read(99, 0, 1, buf) != -EINVAL)
        panic("ahci: bad requests not rejected");
    /* still works after the rejected ones */
    if (ahci_read(td, 63, 1, buf) || !check_sector(buf, 63)) panic("ahci: read after rejects");
    /* the other disk is the vvfat boot ESP: its sector 0 is an MBR */
    int boot = td == 0 ? 1 : 0;
    if (ahci_read(boot, 0, 1, buf) || buf[510] != 0x55 || buf[511] != 0xAA)
        panic("ahci: boot disk MBR signature %x %x", buf[510], buf[511]);
    kfree(buf);
    kprintf("[TEST] ahci: boot disk MBR 55aa ok\n");
    kprintf("[TEST] ahci: disk %d on port %d \"%s\" 64 sectors, lba 0/37/63 + 10..17 verified, bad requests rejected\n",
            td, d->port, d->model);
}

static void test_pci(void) {
    int n = pci_count();
    const struct pci_dev *hb = pci_find_class(0x06, 0x00, 0xFF);
    const struct pci_dev *sata = pci_find_class(0x01, 0x06, 0x01);
    if (n < 3 || !hb || !sata) panic("pci: %d devices, host bridge %p, ahci %p", n, hb, sata);
    if (hb->bus || hb->dev || hb->fn) panic("pci: host bridge not at 00:00.0");
    uint32_t id = pci_read32(sata->bus, sata->dev, sata->fn, 0);
    if ((id & 0xFFFF) != sata->vendor || id >> 16 != sata->device) panic("pci: reread mismatch");
    /* AHCI ABAR is BAR5, a memory BAR (bit 0 clear) */
    if (sata->bar[5] & 1 || !(sata->bar[5] & ~0xFu)) panic("pci: ahci bar5 %x", sata->bar[5]);
    kprintf("[TEST] pci: %d devices, host bridge %04x:%04x, ahci %04x:%04x at %02x:%02x.%x\n",
            n, hb->vendor, hb->device, sata->vendor, sata->device, sata->bus, sata->dev, sata->fn);
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
    pci_scan();
    if (g_test_mode) test_pci();
    ahci_init();
    if (g_test_mode) test_ahci();

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
    {
        int d = fat_find_disk();
        if (d >= 0) {
            int n = fat_mount(d, "/disk");
            if (n < 0) kprintf("fat: mounting disk %d failed (%d)\n", d, n);
        } else {
            kprintf("fat: no FAT16 disk found, /disk not mounted\n");
        }
        if (g_test_mode) test_fat();
    }
    syscall_init();
    proc_init();
    tty_init();
    if (g_test_mode) { test_user(); test_elf(); test_wait(); test_exec(); test_kill(); test_fpu(); test_pipes(); test_fbdev(); }
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
