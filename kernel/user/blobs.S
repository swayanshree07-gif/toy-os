/* Ring-3 test programs, before there is an ELF loader (Day 19): flat, position-
 * independent code blobs that the kernel copies to USER_CODE_BASE. They only use
 * rip-relative addressing and system calls (syscall_nr.h). They live in .rodata:
 * the kernel never executes them, it only copies the bytes. */
#include "syscall_nr.h"

#define BLOB(name)  .balign 16; .global ublob_##name; ublob_##name:
#define BLOB_END(name) .global ublob_##name##_end; ublob_##name##_end:
#define SYSCALL(nr) movl $nr, %eax; syscall
#define EXIT(code)  movl $code, %edi; SYSCALL(SYS_exit)

    .section .rodata.userblobs, "a"

/* Day 15: proves it runs in ring 3, then traps back with #UD (killed, code 128+6). */
BLOB(ring3)
    movl %cs, %eax
    movl %ss, %ebx
    movabs $0x5445524E52334F4B, %r12      /* "TERNR3OK" */
    pushq %r12                            /* the user stack works */
    popq %r13
    ud2
BLOB_END(ring3)

/* Day 16: getpid, write, register preservation, sleep; exits with pid + 1000. */
BLOB(hello)
    SYSCALL(SYS_getpid)
    movq %rax, %r12
    movabs $0x1111111111111111, %rbx
    movabs $0x2222222222222222, %rbp
    movabs $0x3333333333333333, %r13
    movabs $0x4444444444444444, %r14
    movabs $0x5555555555555555, %r15
    call hello_write                      /* a call: uses the user stack */
    cmpq $(hello_msg_end - hello_msg), %rax
    jne 1f
    movabs $0x1111111111111111, %rax
    cmpq %rax, %rbx
    jne 2f
    movabs $0x2222222222222222, %rax
    cmpq %rax, %rbp
    jne 2f
    movabs $0x3333333333333333, %rax
    cmpq %rax, %r13
    jne 2f
    movabs $0x4444444444444444, %rax
    cmpq %rax, %r14
    jne 2f
    movabs $0x5555555555555555, %rax
    cmpq %rax, %r15
    jne 2f
    SYSCALL(SYS_uptime_ms)
    movq %rax, %r13
    movl $30, %edi
    SYSCALL(SYS_sleep_ms)
    SYSCALL(SYS_uptime_ms)
    subq %r13, %rax
    cmpq $30, %rax
    jb 3f
    leaq 1000(%r12), %rdi
    SYSCALL(SYS_exit)
1:  EXIT(1)
2:  EXIT(2)
3:  EXIT(3)
hello_write:
    movl $1, %edi
    leaq hello_msg(%rip), %rsi
    movl $(hello_msg_end - hello_msg), %edx
    SYSCALL(SYS_write)
    ret
hello_msg:
    .ascii "[TEST] syscall write ok: hello from ring 3\n"
hello_msg_end:
BLOB_END(hello)

/* Bad arguments must be rejected with an error, not crash anything. Exits 0 if all ok. */
BLOB(badargs)
    movl $1, %edi                         /* kernel pointer */
    movabs $0xFFFFFFFF80100000, %rsi
    movl $8, %edx
    SYSCALL(SYS_write)
    cmpq $-EFAULT, %rax
    jne 1f
    movl $1, %edi                         /* unmapped user page */
    movl $0x1000, %esi
    movl $4, %edx
    SYSCALL(SYS_write)
    cmpq $-EFAULT, %rax
    jne 2f
    movl $1, %edi                         /* valid start, length runs off the end */
    leaq ublob_badargs(%rip), %rsi
    movabs $0x7FFFFFFFFFFF, %rdx
    SYSCALL(SYS_write)
    cmpq $-EFAULT, %rax
    jne 3f
    movl $7, %edi                         /* bad fd */
    leaq ublob_badargs(%rip), %rsi
    movl $1, %edx
    SYSCALL(SYS_write)
    cmpq $-EBADF, %rax
    jne 4f
    SYSCALL(999)                          /* unknown syscall */
    cmpq $-ENOSYS, %rax
    jne 5f
    movl $1, %edi                         /* zero-length write is fine */
    xorl %esi, %esi
    xorl %edx, %edx
    SYSCALL(SYS_write)
    testq %rax, %rax
    jne 6f
    EXIT(0)
1:  EXIT(1)
2:  EXIT(2)
3:  EXIT(3)
4:  EXIT(4)
5:  EXIT(5)
6:  EXIT(6)
BLOB_END(badargs)

/* Faulting programs: each must be killed while the kernel carries on. */
BLOB(priv)                                /* privileged instruction -> #GP */
    cli
    EXIT(0)
BLOB_END(priv)

BLOB(kread)                               /* read kernel memory -> #PF (protection) */
    movabs $0xFFFFFFFF80100000, %rax
    movq (%rax), %rax
    EXIT(0)
BLOB_END(kread)

BLOB(wcode)                               /* write own code page (read-only) -> #PF */
    leaq ublob_wcode(%rip), %rax
    movb $0x90, (%rax)
    EXIT(0)
BLOB_END(wcode)

/* Busy loop in ring 3 for ~200 ms without yielding: must be preempted. */
BLOB(spin)
    SYSCALL(SYS_uptime_ms)
    movq %rax, %r12
1:  incq %r13
    testl $0xFFFFF, %r13d
    jnz 1b
    SYSCALL(SYS_uptime_ms)
    subq %r12, %rax
    cmpq $200, %rax
    jb 1b
    EXIT(0)
BLOB_END(spin)
