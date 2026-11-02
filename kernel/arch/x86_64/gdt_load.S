/* void gdt_load(const struct gdt_ptr *p, uint16_t code, uint16_t data) */
    .text
    .code64
    .global gdt_load
gdt_load:
    lgdt (%rdi)
    movw %dx, %ds
    movw %dx, %es
    movw %dx, %ss
    xorl %eax, %eax
    movw %ax, %fs
    movw %ax, %gs
    /* reload CS with a far return */
    popq %rax               /* return address */
    movzwq %si, %rsi
    pushq %rsi
    pushq %rax
    lretq
