#ifndef TERN_EXTABLE_H
#define TERN_EXTABLE_H
/* Exception table entries: pairs (faulting instruction address, fixup address).
 * Use from assembly:   1: movq (%rdi), %rax
 *                      EXTABLE_ENTRY(1b, fixup_label)                                */
#ifdef __ASSEMBLER__
#define EXTABLE_ENTRY(insn, fixup) \
    .pushsection .extable, "a"; .balign 8; .quad insn, fixup; .popsection
#else
#include <stdint.h>
struct extable_entry { uint64_t insn, fixup; };
/* Test helpers (extable_test.S). Return the value read, or -1 / 1 on fault. */
long probe_read64(const void *addr, uint64_t *out);   /* 0 ok, -1 fault */
#endif
#endif
