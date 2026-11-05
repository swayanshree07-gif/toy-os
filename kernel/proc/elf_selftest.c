/* Tests for the ELF loader and C user programs from the initrd (Day 19). */
#include "proc.h"
#include "vmm.h"
#include "pmm.h"
#include "mm.h"
#include "vfs.h"
#include "heap.h"
#include "string.h"
#include "kernel.h"
#include "kprintf.h"
#include "ktest.h"

static int run_elf(const char *path, const char *const *argv, struct process *info) {
    int err = 0;
    struct process *p = proc_spawn_elf(path, argv, &err);
    if (!p) panic("elf: spawning %s failed (%d)", path, err);
    return proc_wait(p, info);
}

static int spawn_err(const char *path) {
    int err = 0;
    struct process *p = proc_spawn_elf(path, NULL, &err);
    if (p) panic("elf: %s should not load", path);
    return err;
}

void test_elf(void) {
    const char *argv[] = { "hello", "one", "two words", NULL };
    struct process info;
    /* warm-up run: lets the kernel heap grow to its working size first */
    if (run_elf("/bin/hello", argv, &info) != 42) panic("elf: hello warm-up failed");
    uint64_t frames0 = pmm_free_frames();

    int code = run_elf("/bin/hello", argv, &info);
    if (code != 42 || info.faulted) panic("elf: hello exit %d faulted %d", code, info.faulted);
    /* text, rodata and data+bss (8 KiB of bss) on their own pages, image under 1 MiB */
    if (info.entry != USER_CODE_BASE || info.brk_base % PAGE_SIZE ||
        info.brk_base < USER_CODE_BASE + 5 * PAGE_SIZE ||
        info.brk_base > USER_CODE_BASE + (1u << 20))
        panic("elf: hello entry %lx brk %lx", info.entry, info.brk_base);

    code = run_elf("/bin/fstest", NULL, &info);
    if (code != 0) panic("elf: fstest failed check %d", code);

    code = run_elf("/bin/ulibtest", NULL, &info);
    if (code != 0) panic("elf: ulibtest failed check %d", code);

    const char *segv_argv[] = { "segv", NULL };
    code = run_elf("/bin/segv", segv_argv, &info);
    if (code != PROC_FAULT_EXIT_BASE + 14 || !info.faulted || info.fault_cr2 != 0 ||
        (info.fault_regs.error & 6) != 6)                /* write, user */
        panic("elf: segv exit %d cr2 %lx err %lx", code, info.fault_cr2, info.fault_regs.error);

    /* loader errors */
    int e1 = spawn_err("/bin/nope");
    int e2 = spawn_err("/bin");
    kassert(vfs_write_file("/bin/garbage", "#!/bin/sh\necho no\n", 18) == 18);
    int e3 = spawn_err("/bin/garbage");
    kassert(vfs_unlink(NULL, "/bin/garbage") == 0);
    if (e1 != -ENOENT || e2 != -EACCES || e3 != -ENOEXEC)
        panic("elf: error codes %d %d %d", e1, e2, e3);

    if (pmm_free_frames() != frames0)
        panic("elf: frames leaked: %lu -> %lu", frames0, pmm_free_frames());
    kprintf("[TEST] elf loader ok: /bin/hello exit 42, /bin/fstest + /bin/ulibtest passed, "
            "/bin/segv killed, "
            "ENOENT/EACCES/ENOEXEC rejected, no frames leaked\n");
}

/* ---- unit tests: malformed images are rejected before anything is mapped ---- */

struct test_elf {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
    struct { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } ph[2];
    uint8_t code[16];
};

static void make_valid(struct test_elf *e) {
    memset(e, 0, sizeof *e);
    memcpy(e->ident, "\x7f" "ELF\x02\x01\x01", 7);
    e->type = 2; e->machine = 62; e->version = 1;
    e->entry = 0x400000;
    e->phoff = 64;
    e->ehsize = 64; e->phentsize = 56; e->phnum = 2;
    e->ph[0].type = 1; e->ph[0].flags = 5;              /* R X */
    e->ph[0].offset = __builtin_offsetof(struct test_elf, code);
    e->ph[0].vaddr = 0x400000; e->ph[0].filesz = 16; e->ph[0].memsz = 16;
    e->ph[1].type = 1; e->ph[1].flags = 6;              /* R W, bss only */
    e->ph[1].vaddr = 0x401000; e->ph[1].memsz = 0x2000;
    memset(e->code, 0x90, sizeof e->code);
}

static int try_load(const struct test_elf *e, size_t len, uint64_t *brk) {
    uint64_t s = vmm_new_space(), entry = 0, b = 0;
    int rc = elf_load(s, e, len, &entry, &b);
    if (rc == 0 && brk) *brk = b;
    if (rc != 0 && vmm_user_pages(s) != 0) rc = 999;     /* nothing may be mapped */
    vmm_destroy_space(s);
    return rc;
}

KTEST(elf_loader_validation) {
    static struct test_elf e;
    uint64_t brk = 0;
    make_valid(&e);
    KEXPECT(try_load(&e, sizeof e, &brk) == 0 && brk == 0x403000);
#define BAD(mutation) do { make_valid(&e); mutation; \
        KEXPECT(try_load(&e, sizeof e, NULL) == -ENOEXEC); } while (0)
    BAD(e.ident[0] = 0);                         /* magic */
    BAD(e.ident[4] = 1);                         /* 32-bit */
    BAD(e.machine = 3);                          /* i386 */
    BAD(e.type = 3);                             /* ET_DYN */
    BAD(e.phentsize = 32);
    BAD(e.phnum = 0);
    BAD(e.phoff = sizeof e);                     /* headers past the end */
    BAD(e.ph[0].filesz = 17; e.ph[0].memsz = 8);  /* filesz > memsz */
    BAD(e.ph[0].offset = sizeof e - 4);          /* data past the end */
    BAD(e.ph[0].vaddr = 0x400800);               /* unaligned */
    BAD(e.ph[0].vaddr = 0; e.entry = 0);         /* NULL page */
    BAD(e.ph[1].vaddr = 0xFFFFFFFF80000000ull);  /* kernel half */
    BAD(e.ph[1].memsz = ~0ull - 0x1000);         /* wraps */
    BAD(e.ph[0].flags = 7);                      /* W+X */
    BAD(e.ph[1].vaddr = 0x400000);               /* overlap */
    BAD(e.entry = 0x401000);                     /* entry not executable */
    BAD(e.ph[0].type = 0; e.ph[1].type = 0);     /* no PT_LOAD */
    make_valid(&e);
    KEXPECT(try_load(&e, 40, NULL) == -ENOEXEC); /* truncated header */
#undef BAD
}
