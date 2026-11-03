/* Interrupt entry stubs. Each of the 256 stubs is 16-byte aligned so the IDT setup
 * can compute stub addresses as isr_stubs + 16*vector. Stubs push a dummy error code
 * for vectors where the CPU doesn't push one, then the vector number. */
    .text
    .code64

    .balign 16
    .global isr_stubs
isr_stubs:
    .set vec, 0
    .rept 256
    .balign 16
    .if (vec == 8) || (vec == 10) || (vec == 11) || (vec == 12) || (vec == 13) || (vec == 14) || (vec == 17) || (vec == 21) || (vec == 29) || (vec == 30)
    .else
    pushq $0
    .endif
    pushq $vec
    jmp isr_common
    .set vec, vec + 1
    .endr
    .balign 16
    .global isr_stubs_end
isr_stubs_end:

isr_common:
    cld
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, %rdi
    /* align the stack to 16 for the C call (frame may be 8 mod 16) */
    movq %rsp, %rbx
    andq $-16, %rsp
    call isr_dispatch
    movq %rbx, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp          /* vector + error code */
    iretq
