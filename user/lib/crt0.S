/* User program entry. The kernel starts us with rsp -> argc, argv..., NULL, envp NULL,
 * auxv (System V layout), rsp 16-byte aligned, all other registers zero. */
#include "syscall_nr.h"
    .section .text._start, "ax"
    .global _start
_start:
    xorl %ebp, %ebp              /* end of the frame-pointer chain */
    movq (%rsp), %rdi            /* argc */
    leaq 8(%rsp), %rsi           /* argv */
    call main
    movl %eax, %edi
    movl $SYS_exit, %eax
    syscall
    ud2
