#ifndef TERN_ENTRY_H
#define TERN_ENTRY_H
/* C functions that are only called from assembly (boot stub, ISR stubs,
 * syscall entry, new-thread trampoline). Declared here so every C definition
 * has a prototype (-Wmissing-prototypes) and the signatures stay in one place. */
#include <stdint.h>
#include "idt.h"
#include "sched.h"

void kmain(uint32_t magic, uint32_t info_addr);              /* boot/boot.S */
void isr_dispatch(struct regs *r);                           /* arch/x86_64/isr.S */
void syscall_dispatch(struct regs *r);                       /* syscall entry stub */
__attribute__((noreturn)) void thread_bootstrap(thread_fn fn, void *arg); /* switch.S */
#endif
