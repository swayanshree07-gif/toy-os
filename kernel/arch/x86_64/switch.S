/* Kernel thread context switch.
 *
 * void context_switch(uint64_t *save_rsp, uint64_t new_rsp)
 *   Pushes the callee-saved registers on the current stack, stores rsp in *save_rsp,
 *   loads new_rsp and pops the other thread's callee-saved registers. The return
 *   address on the new stack is either the place where that thread called
 *   context_switch, or thread_entry for a brand-new thread (see sched.c). */
    .text
    .code64
    .global context_switch
context_switch:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, (%rdi)
    movq %rsi, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret

/* First "return" of a new thread: r12 = fn, r13 = arg (set up by thread_create).
 * rsp is 16-byte aligned here, as the C ABI expects before a call. */
    .global thread_entry
thread_entry:
    xorl %ebp, %ebp
    movq %r12, %rdi
    movq %r13, %rsi
    call thread_bootstrap
    ud2
