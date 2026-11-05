/* User processes: address space setup, ring-3 entry, fault handling. */
#include "proc.h"
#include "sched.h"
#include "vmm.h"
#include "pmm.h"
#include "mm.h"
#include "heap.h"
#include "kernel.h"
#include "kprintf.h"
#include "string.h"

extern __attribute__((noreturn)) void enter_user(uint64_t rip, uint64_t rsp);

static int next_pid = 1;

struct process *proc_current(void) {
    struct thread *t = thread_current();
    return t ? t->proc : NULL;
}

static void user_thread_start(void *arg) {
    struct process *p = arg;
    /* The scheduler already loaded p->pml4 into CR3 when it switched to us. */
    enter_user(p->entry, p->stack_top);
}

/* Copy len bytes into the (already mapped) user range at va of another address space. */
static void copy_to_space(uint64_t pml4, uint64_t va, const void *src, size_t len) {
    const uint8_t *s = src;
    while (len) {
        uint64_t pa;
        kassert(vmm_translate(pml4, va, &pa, NULL));
        size_t chunk = PAGE_SIZE - (va & (PAGE_SIZE - 1));
        if (chunk > len) chunk = len;
        memcpy(phys_to_virt(pa), s, chunk);
        va += chunk; s += chunk; len -= chunk;
    }
}

struct process *proc_spawn_blob(const char *name, const void *code, size_t len) {
    struct process *p = kzalloc(sizeof *p);
    if (!p) return NULL;
    p->pml4 = vmm_new_space();
    if (!p->pml4) goto fail;
    uint64_t code_pages = ALIGN_UP(len, PAGE_SIZE) / PAGE_SIZE;
    /* code: user, read + execute (no PTE_W, no NX) */
    if (vmm_map_user_pages(p->pml4, USER_CODE_BASE, code_pages, 0)) goto fail;
    copy_to_space(p->pml4, USER_CODE_BASE, code, len);
    if (vmm_map_user_pages(p->pml4, USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE,
                           USER_STACK_PAGES, PTE_W | PTE_NX))
        goto fail;
    p->entry = USER_CODE_BASE;
    p->stack_top = USER_STACK_TOP;
    for (unsigned i = 0; i < sizeof p->name - 1 && name[i]; i++) p->name[i] = name[i];

    /* The new thread must not run before it knows its address space. */
    uint64_t f = irq_save();
    p->pid = next_pid++;
    p->thread = thread_create(name, user_thread_start, p);
    if (!p->thread) { irq_restore(f); goto fail; }
    p->thread->proc = p;
    p->thread->cr3 = p->pml4;
    irq_restore(f);
    return p;
fail:
    if (p->pml4) vmm_destroy_space(p->pml4);
    kfree(p);
    return NULL;
}

void proc_thread_reaped(struct thread *t) {
    struct process *p = t->proc;
    p->ticks_run = t->ticks_run;
    p->switches = t->switches;
    if (p->pml4) {
        vmm_destroy_space(p->pml4);
        p->pml4 = 0;
    }
}

int proc_wait(struct process *p, struct process *info) {
    int code = thread_join(p->thread);
    kassert(p->pml4 == 0);
    p->thread = NULL;
    if (info) *info = *p;
    kfree(p);
    return code;
}

__attribute__((noreturn)) void proc_user_fault(struct regs *r) {
    struct process *p = proc_current();
    if (!p) panic("ring-3 exception %lu without a process", r->vector);
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    p->faulted = true;
    p->fault_regs = *r;
    p->fault_cr2 = cr2;
    kprintf("user fault: pid %d '%s': %s (vector %lu, error %lx) at rip %lx",
            p->pid, p->name, exception_name(r->vector), r->vector, r->error, r->rip);
    if (r->vector == 14) kprintf(" cr2 %lx", cr2);
    kprintf(" -> killed\n");
    thread_exit(PROC_FAULT_EXIT_BASE + (int)r->vector);
}
