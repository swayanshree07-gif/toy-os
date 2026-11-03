/* Fault-probing helpers that use the exception table. */
#include "extable.h"
    .text
    .code64

/* long probe_read64(const void *addr, uint64_t *out): 0 and *out = value, or -1 on fault */
    .global probe_read64
probe_read64:
1:  movq (%rdi), %rax
    movq %rax, (%rsi)
    xorl %eax, %eax
    ret
2:  movq $-1, %rax
    ret
    EXTABLE_ENTRY(1b, 2b)

/* long probe_write64(void *addr, uint64_t value): 0 ok, -1 on fault */
    .global probe_write64
probe_write64:
1:  movq %rsi, (%rdi)
    xorl %eax, %eax
    ret
2:  movq $-1, %rax
    ret
    EXTABLE_ENTRY(1b, 2b)
