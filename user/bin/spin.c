/* /bin/spin [MS]: busy-loop in ring 3 (forever, or for MS milliseconds), for testing
 * preemption, kill and Ctrl-C. Prints "spin: started" first. */
#include "tern.h"

int main(int argc, char **argv) {
    uint64_t ms = argc > 1 ? (uint64_t)atoi(argv[1]) : 0;
    printf("spin: started (pid %d, %lu ms)\n", getpid(), (unsigned long)ms);
    uint64_t end = uptime_ms() + ms;
    volatile uint64_t n = 0;
    while (!ms || uptime_ms() < end)
        for (int i = 0; i < 100000; i++) n++;
    return 0;
}
