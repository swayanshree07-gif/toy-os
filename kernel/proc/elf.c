/* ELF64 loader for user programs (Day 19).
 *
 * Accepts static, non-PIE x86-64 executables (ET_EXEC). Every PT_LOAD segment must lie
 * in [USER_CODE_BASE, USER_LOAD_TOP), be page-aligned in memory (user/linker.ld puts
 * each segment on its own pages), not overlap another segment and not be both
 * writable and executable (W^X). The file is validated completely before anything is
 * mapped; mapping failures leave pages behind that vmm_destroy_space frees. */
#include "proc.h"
#include "vmm.h"
#include "mm.h"
#include "syscall_nr.h"
#include "string.h"
#include "kprintf.h"

#define EI_NIDENT 16
#define ET_EXEC 2
#define EM_X86_64 62
#define PT_LOAD 1
#define PF_X 1
#define PF_W 2
#define ELF_MAX_PHDRS 16
/* Program images must end well below the stack. */
#define USER_LOAD_TOP 0x0000100000000000ull

struct elf64_ehdr {
    uint8_t  ident[EI_NIDENT];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct elf64_phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

_Static_assert(sizeof(struct elf64_ehdr) == 64, "ehdr size");
_Static_assert(sizeof(struct elf64_phdr) == 56, "phdr size");

static int validate(const uint8_t *img, size_t len, const struct elf64_phdr **ph_out,
                    unsigned *nph) {
    if (len < sizeof(struct elf64_ehdr)) return -ENOEXEC;
    const struct elf64_ehdr *eh = (const void *)img;
    if (memcmp(eh->ident, "\x7f" "ELF", 4)) return -ENOEXEC;
    if (eh->ident[4] != 2 || eh->ident[5] != 1 || eh->ident[6] != 1)  /* 64-bit, LE, v1 */
        return -ENOEXEC;
    if (eh->type != ET_EXEC || eh->machine != EM_X86_64 || eh->version != 1) return -ENOEXEC;
    if (eh->phentsize != sizeof(struct elf64_phdr) || eh->phnum == 0 ||
        eh->phnum > ELF_MAX_PHDRS)
        return -ENOEXEC;
    if (eh->phoff > len || (uint64_t)eh->phnum * sizeof(struct elf64_phdr) > len - eh->phoff)
        return -ENOEXEC;
    if (eh->phoff % 8) return -ENOEXEC;
    const struct elf64_phdr *ph = (const void *)(img + eh->phoff);
    bool entry_ok = false;
    unsigned loads = 0;
    for (unsigned i = 0; i < eh->phnum; i++) {
        const struct elf64_phdr *a = &ph[i];
        if (a->type != PT_LOAD || a->memsz == 0) continue;
        loads++;
        if (a->filesz > a->memsz) return -ENOEXEC;
        if (a->offset > len || a->filesz > len - a->offset) return -ENOEXEC;
        if (a->vaddr % PAGE_SIZE) return -ENOEXEC;
        if (a->vaddr < USER_CODE_BASE || a->vaddr >= USER_LOAD_TOP ||
            a->memsz > USER_LOAD_TOP - a->vaddr)
            return -ENOEXEC;
        if ((a->flags & PF_W) && (a->flags & PF_X)) return -ENOEXEC;    /* W^X */
        uint64_t end = ALIGN_UP(a->vaddr + a->memsz, PAGE_SIZE);
        for (unsigned j = 0; j < i; j++) {
            const struct elf64_phdr *b = &ph[j];
            if (b->type != PT_LOAD || b->memsz == 0) continue;
            uint64_t bend = ALIGN_UP(b->vaddr + b->memsz, PAGE_SIZE);
            if (a->vaddr < bend && b->vaddr < end) return -ENOEXEC;     /* overlap */
        }
        if ((a->flags & PF_X) && eh->entry >= a->vaddr && eh->entry < a->vaddr + a->memsz)
            entry_ok = true;
    }
    if (!loads || !entry_ok) return -ENOEXEC;
    *ph_out = ph;
    *nph = eh->phnum;
    return 0;
}

int elf_load(uint64_t pml4, const void *image, size_t len, uint64_t *entry, uint64_t *brk) {
    const uint8_t *img = image;
    const struct elf64_phdr *ph;
    unsigned nph;
    int rc = validate(img, len, &ph, &nph);
    if (rc) return rc;
    uint64_t top = 0;
    for (unsigned i = 0; i < nph; i++) {
        const struct elf64_phdr *a = &ph[i];
        if (a->type != PT_LOAD || a->memsz == 0) continue;
        uint64_t flags = 0;
        if (a->flags & PF_W) flags |= PTE_W;
        if (!(a->flags & PF_X)) flags |= PTE_NX;
        uint64_t pages = ALIGN_UP(a->memsz, PAGE_SIZE) / PAGE_SIZE;
        if (vmm_map_user_pages(pml4, a->vaddr, pages, flags)) return -ENOMEM;
        /* frames are zeroed, so .bss (memsz > filesz) needs nothing more */
        proc_copy_to_space(pml4, a->vaddr, img + a->offset, a->filesz);
        uint64_t end = a->vaddr + pages * PAGE_SIZE;
        if (end > top) top = end;
    }
    *entry = ((const struct elf64_ehdr *)img)->entry;
    *brk = top;
    return 0;
}
