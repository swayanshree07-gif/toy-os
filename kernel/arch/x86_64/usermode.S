/* Ring 3 entry and the `syscall` entry point.
 *
 * Single CPU, so no swapgs/per-CPU area: syscall_entry runs with IF=0 (SFMASK clears
 * it), parks the user rsp in a global, switches to the current thread's kernel stack
 * (g_syscall_kstack, kept equal to TSS.rsp0 by the scheduler) and builds a
 * `struct regs` frame identical to the one isr.S builds, so syscall handlers, the
 * scheduler and the user-fault code see one frame layout. */
#include "gdt.h"
    .text
    .code64

/* noreturn void enter_user(uint64_t rip, uint64_t rsp): iretq to ring 3 with IF=1 and
 * all general-purpose registers zeroed (no kernel values leak to user space). */
    .global enter_user
enter_user:
    cli
    pushq $(GDT_UDATA | 3)     /* ss */
    pushq %rsi                 /* rsp */
    pushq $0x202               /* rflags: IF */
    pushq $(GDT_UCODE | 3)     /* cs */
    pushq %rdi                 /* rip */
    movw $(GDT_UDATA | 3), %ax
    movw %ax, %ds
    movw %ax, %es
    xorl %eax, %eax
    xorl %ebx, %ebx
    xorl %ecx, %ecx
    xorl %edx, %edx
    xorl %esi, %esi
    xorl %edi, %edi
    xorl %ebp, %ebp
    xorl %r8d, %r8d
    xorl %r9d, %r9d
    xorl %r10d, %r10d
    xorl %r11d, %r11d
    xorl %r12d, %r12d
    xorl %r13d, %r13d
    xorl %r14d, %r14d
    xorl %r15d, %r15d
    iretq

/* noreturn void return_to_user(struct regs *r): restore a full register frame (the
 * layout syscall_entry builds) and iretq to ring 3. Used by fork's child (Day 22). */
    .global return_to_user
return_to_user:
    cli
    movq %rdi, %rsp
    movw $(GDT_UDATA | 3), %ax
    movw %ax, %ds
    movw %ax, %es
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
    addq $16, %rsp             /* vector, error */
    iretq

    .global syscall_entry
syscall_entry:
    movq %rsp, syscall_user_rsp(%rip)
    movq g_syscall_kstack(%rip), %rsp
    pushq $(GDT_UDATA | 3)             /* ss */
    pushq syscall_user_rsp(%rip)       /* rsp */
    pushq %r11                         /* rflags (saved by syscall) */
    pushq $(GDT_UCODE | 3)             /* cs */
    pushq %rcx                         /* rip (saved by syscall) */
    pushq $0                           /* error */
    pushq $0x100                       /* "vector" 0x100 marks a syscall frame */
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
    movq %rsp, %rdi            /* 22 qwords below a 16-aligned stack top: aligned */
    call syscall_dispatch      /* returns with IF=0 and a sysret-safe frame */
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
    addq $16, %rsp             /* vector, error */
    popq %rcx                  /* rip */
    addq $8, %rsp              /* cs */
    popq %r11                  /* rflags */
    popq %rsp                  /* user rsp (ss is implied by STAR) */
    sysretq

    .bss
    .balign 8
syscall_user_rsp:
    .quad 0
