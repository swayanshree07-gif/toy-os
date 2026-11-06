/* /bin/ps: list processes (pid, parent, state, CPU time, heap pages, name). */
#include "tern.h"

int main(void) {
    static struct tern_procinfo pi[64];
    static const char *const st[] = { "run", "ready", "sleep", "block", "exit" };
    int n = proclist(pi, 64);
    if (n < 0) { printf("ps: error %d\n", n); return 1; }
    if (n > 64) n = 64;
    printf("  PID  PPID STATE    TIME HEAP NAME\n");
    for (int i = n - 1; i >= 0; i--) {          /* the kernel lists newest first */
        struct tern_procinfo *p = &pi[i];
        unsigned long ms = (unsigned long)p->ticks * 10;
        printf("%5d %5d %-5s %3lu.%02lu %4lu %s\n", p->pid, p->ppid,
               p->state <= TERN_PS_EXITED ? st[p->state] : "?", ms / 1000,
               (ms % 1000) / 10, (unsigned long)p->mem_pages, p->name);
    }
    return 0;
}
