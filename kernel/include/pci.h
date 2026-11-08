#ifndef TERN_PCI_H
#define TERN_PCI_H
#include <stdint.h>

/* PCI configuration space through the legacy 0xCF8/0xCFC ports (mechanism #1). */
struct pci_dev {
    uint8_t bus, dev, fn;
    uint16_t vendor, device;
    uint8_t class_code, subclass, prog_if;
    uint8_t irq_line;
    uint32_t bar[6];
};

#define PCI_MAX_DEVS 32

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v);
/* Enumerate everything reachable from bus 0 (bridges followed); returns the count. */
int pci_scan(void);
int pci_count(void);
const struct pci_dev *pci_get(int i);
/* First device with this class/subclass (prog_if 0xFF = any), or NULL. */
const struct pci_dev *pci_find_class(uint8_t cls, uint8_t sub, uint8_t prog_if);
const char *pci_class_name(uint8_t cls, uint8_t sub);
#endif
