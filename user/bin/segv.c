/* /bin/segv: writes through a NULL pointer. The kernel must kill only this process
 * (exit code 128 + 14) and keep running. */
#include "tern.h"

int main(int argc, char **argv) {
    (void)argv;
    volatile int *p = (volatile int *)(uintptr_t)(argc - 1);   /* 0, hidden from the compiler */
    *p = 1;
    puts("segv: still alive?!");
    return 0;
}
