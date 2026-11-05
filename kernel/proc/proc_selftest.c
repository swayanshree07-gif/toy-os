/* Boot-time tests for ring 3, system calls and user fault isolation (Days 15-16). */
#include "proc.h"
#include "sched.h"
#include "vmm.h"
#include "pmm.h"
#include "mm.h"
#include "gdt.h"
#include "timer.h"
#include "kernel.h"
#include "kprintf.h"
#include "ktest.h"

#define DECLARE_BLOB(n) extern const char ublob_##n[], ublob_##n##_end[]
DECLARE_BLOB(ring3); DECLARE_BLOB(hello); DECLARE_BLOB(badargs);
DECLARE_BLOB(priv); DECLARE_BLOB(kread); DECLARE_BLOB(wcode); DECLARE_BLOB(spin);
#define SPAWN(n) proc_spawn_blob(#n, ublob_##n, (size_t)(ublob_##n##_end - ublob_##n))

static struct process *spawn_checked(struct process *p, const char *what) {
    if (!p) panic("user: spawning %s failed", what);
    return p;
}

/* Wait for one program; its final process record is copied to *out. */
static int run(struct process *p, struct process *out) { return proc_wait(p, out); }

static void test_ring3(void) {
    struct process r;
    int code = run(spawn_checked(SPAWN(ring3), "ring3"), &r);
    const struct regs *f = &r.fault_regs;
    if (code != PROC_FAULT_EXIT_BASE + 6 || !r.faulted || f->vector != 6)
        panic("ring3: exit %d faulted %d vector %lu", code, r.faulted, f->vector);
    if (f->cs != (GDT_UCODE | 3) || f->ss != (GDT_UDATA | 3) ||
        (f->rax & 0xFFFF) != (GDT_UCODE | 3) || (f->rbx & 0xFFFF) != (GDT_UDATA | 3))
        panic("ring3: cs %lx ss %lx rax %lx rbx %lx", f->cs, f->ss, f->rax, f->rbx);
    if (f->r13 != 0x5445524E52334F4Bull || f->rip < USER_CODE_BASE ||
        f->rip >= USER_CODE_BASE + PAGE_SIZE || f->rsp != USER_STACK_TOP)
        panic("ring3: r13 %lx rip %lx rsp %lx", f->r13, f->rip, f->rsp);
    kprintf("[TEST] ring3 ok: ran at cpl 3 (cs=%lx ss=%lx), user stack ok, "
            "#UD at %lx trapped and killed\n", f->cs, f->ss, f->rip);
}

static void test_syscalls(void) {
    struct process r;
    struct process *p = spawn_checked(SPAWN(hello), "hello");
    int pid = p->pid;
    int code = run(p, &r);
    if (code != pid + 1000) panic("syscall hello: exit %d (pid %d, wanted %d)", code, pid, pid + 1000);
    kprintf("[TEST] syscall ok: getpid %d, write, sleep_ms, regs preserved, exit %d\n", pid, code);

    code = run(spawn_checked(SPAWN(badargs), "badargs"), &r);
    if (code != 0) panic("syscall badargs: check %d failed", code);
    kprintf("[TEST] syscall bad args rejected: EFAULT x3, EBADF, ENOSYS\n");
}

static void test_user_faults(void) {
    struct process r;
    int code = run(spawn_checked(SPAWN(priv), "priv"), &r);
    if (code != PROC_FAULT_EXIT_BASE + 13 || r.fault_regs.vector != 13)
        panic("user priv: exit %d", code);
    code = run(spawn_checked(SPAWN(kread), "kread"), &r);
    if (code != PROC_FAULT_EXIT_BASE + 14 || r.fault_cr2 != 0xFFFFFFFF80100000ull ||
        (r.fault_regs.error & 7) != 5)          /* protection, read, user */
        panic("user kread: exit %d cr2 %lx err %lx", code, r.fault_cr2, r.fault_regs.error);
    code = run(spawn_checked(SPAWN(wcode), "wcode"), &r);
    if (code != PROC_FAULT_EXIT_BASE + 14 || r.fault_cr2 != USER_CODE_BASE ||
        (r.fault_regs.error & 7) != 7)          /* protection, write, user */
        panic("user wcode: exit %d cr2 %lx err %lx", code, r.fault_cr2, r.fault_regs.error);
    kprintf("[TEST] user faults isolated: #GP, kernel read #PF, code write #PF killed\n");
}

static void test_user_preempt(void) {
    struct process *a = spawn_checked(SPAWN(spin), "spin-a");
    struct process *b = spawn_checked(SPAWN(spin), "spin-b");
    uint64_t t0 = timer_ms();
    struct process ra, rb;
    int ca = run(a, &ra), cb = run(b, &rb);
    uint64_t ms = timer_ms() - t0;
    if (ca || cb) panic("user spin: exit %d %d", ca, cb);
    /* Both ran ~200 ms of wall time concurrently, so the whole thing takes ~200 ms,
     * not 400, and both were interrupted in ring 3 many times. */
    if (ms > 350 || ra.user_irqs < 5 || rb.user_irqs < 5 || ra.switches < 3 || rb.switches < 3)
        panic("user spin: %lu ms, irqs %lu/%lu switches %lu/%lu", ms, ra.user_irqs,
              rb.user_irqs, ra.switches, rb.switches);
    kprintf("[TEST] user preempt ok: 2 ring-3 spinners in %lu ms, %lu/%lu irqs in ring 3, "
            "%lu/%lu switches\n", ms, ra.user_irqs, rb.user_irqs, ra.switches, rb.switches);
}

void test_user(void) {
    uint64_t frames0 = pmm_free_frames();
    test_ring3();
    test_syscalls();
    test_user_faults();
    test_user_preempt();
    if (pmm_free_frames() != frames0)
        panic("user: frames leaked: %lu -> %lu", frames0, pmm_free_frames());
    if ((read_cr3() & PTE_ADDR) != vmm_kernel_pml4()) panic("user: cr3 not restored");
    kprintf("[TEST] user processes freed: address spaces and frames back to baseline\n");
}

KTEST(user_address_space) {
    uint64_t frames0 = pmm_free_frames();
    uint64_t s = vmm_new_space();
    KEXPECT(s != 0);
    uint64_t *pml4 = phys_to_virt(s), *k = phys_to_virt(vmm_kernel_pml4());
    KEXPECT(pml4[0] == 0 && pml4[256] == k[256] && pml4[511] == k[511]);
    KEXPECT(vmm_map_user_pages(s, 0x400000, 2, 0) == 0);
    KEXPECT(vmm_map_user_pages(s, 0x7FFFFFFFF000ull, 1, PTE_W | PTE_NX) == 0);
    KEXPECT(vmm_map_user_pages(s, USER_TOP, 1, 0) != 0);     /* not user space */
    KEXPECT(vmm_user_pages(s) == 3);
    KEXPECT(vmm_user_range_ok(s, 0x400000, 2 * PAGE_SIZE, false));
    KEXPECT(!vmm_user_range_ok(s, 0x400000, 2 * PAGE_SIZE + 1, false));   /* runs off */
    KEXPECT(!vmm_user_range_ok(s, 0x400000, 8, true));                    /* read-only */
    KEXPECT(vmm_user_range_ok(s, 0x7FFFFFFFFFF0ull, 16, true));
    KEXPECT(!vmm_user_range_ok(s, 0x7FFFFFFFFFF0ull, 17, true));          /* non-canonical */
    KEXPECT(!vmm_user_range_ok(s, ~0ull - 4, 8, false));                   /* wraps */
    KEXPECT(vmm_user_range_ok(s, 0x1234, 0, false));                       /* empty */
    /* kernel memory is mapped in every space, but not for the user */
    KEXPECT(!vmm_user_range_ok(s, (uint64_t)__kernel_start, 8, false));
    vmm_destroy_space(s);
    KEXPECT(pmm_free_frames() == frames0);
}
