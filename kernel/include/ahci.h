#ifndef TERN_AHCI_H
#define TERN_AHCI_H
#include <stdint.h>
#include <stdbool.h>

/* AHCI (SATA) driver, polled, no interrupts. Only ATA disks (signature 0x101) are used;
 * ATAPI drives are listed but skipped. */
struct ahci_disk {
    int port;
    uint64_t sectors;          /* from IDENTIFY words 100-103 (LBA48) */
    char model[41];
};

int ahci_init(void);                       /* returns number of ATA disks found */
int ahci_disk_count(void);
const struct ahci_disk *ahci_disk_get(int i);
/* Read `count` (1..8) sectors at `lba` into buf (512*count bytes). 0 or -EIO/-EINVAL. */
int ahci_read(int disk, uint64_t lba, unsigned count, void *buf);
/* Write count (1..8) sectors from buf; polled, returns 0 or -EIO/-EINVAL. */
int ahci_write(int disk, uint64_t lba, unsigned count, const void *buf);
int ahci_flush(int disk);                  /* FLUSH CACHE EXT */
#endif
