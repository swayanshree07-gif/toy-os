/* /bin/lsblk: list the AHCI disks, their size and where a FAT volume is mounted. */
#include "tern.h"

int main(void) {
    static struct tern_blkdev d[8];
    int n = blklist(d, 8);
    if (n < 0) { printf("lsblk: error %d\n", n); return 1; }
    printf("NAME   PORT    SECTORS   SIZE  MODEL                    MOUNT\n");
    for (int i = 0; i < n && i < 8; i++) {
        uint64_t kib = d[i].sectors / 2;
        if (kib >= 10240) printf("disk%u %5u %10lu %4lu M  %-24s %s\n", d[i].index, d[i].port,
                                 (unsigned long)d[i].sectors, (unsigned long)(kib / 1024),
                                 d[i].model, d[i].mount[0] ? d[i].mount : "-");
        else printf("disk%u %5u %10lu %4lu K  %-24s %s\n", d[i].index, d[i].port,
                    (unsigned long)d[i].sectors, (unsigned long)kib, d[i].model,
                    d[i].mount[0] ? d[i].mount : "-");
    }
    printf("lsblk: %d disk%s\n", n, n == 1 ? "" : "s");
    return 0;
}
