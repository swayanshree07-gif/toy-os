/* /bin/lspci: list the PCI devices the kernel found at boot. */
#include "tern.h"

int main(void) {
    static struct tern_pcidev d[64];
    int n = pcilist(d, 64);
    if (n < 0) { printf("lspci: error %d\n", n); return 1; }
    for (int i = 0; i < n && i < 64; i++)
        printf("%02x:%02x.%x %04x:%04x class %02x%02x%02x %-12s irq %u\n", d[i].bus, d[i].dev,
               d[i].fn, d[i].vendor, d[i].device, d[i].class_code, d[i].subclass,
               d[i].prog_if, d[i].kind, d[i].irq);
    printf("lspci: %d devices\n", n);
    return 0;
}
