/* /bin/poweroff: switch the machine off (ACPI S5; QEMU exits). */
#include "tern.h"

int main(void) {
    printf("poweroff: bye\n");
    int rc = poweroff();
    printf("poweroff: failed (%d)\n", rc);
    return 1;
}
