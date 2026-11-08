/* PCI enumeration (Day 13 session, start of re-plan session 14).
 * Config mechanism #1: write the address (enable bit, bus, device, function, dword
 * offset) to 0xCF8, then read/write the dword at 0xCFC. The scan starts at bus 0 and
 * follows PCI-to-PCI bridges (class 06/04) into their secondary bus, so it only visits
 * buses that exist. Multi-function devices (header type bit 7) get functions 1-7. */
#include "pci.h"
#include "io.h"
#include "kprintf.h"
#include "sync.h"

static struct pci_dev devs[PCI_MAX_DEVS];
static int ndevs;
static struct spinlock pci_lock = SPINLOCK_INIT("pci");

static uint32_t addr(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    return 0x80000000u | (uint32_t)bus << 16 | (uint32_t)(dev & 31) << 11 |
           (uint32_t)(fn & 7) << 8 | (off & 0xFC);
}

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    uint64_t f = spin_lock_irqsave(&pci_lock);
    outl(0xCF8, addr(bus, dev, fn, off));
    uint32_t v = inl(0xCFC);
    spin_unlock_irqrestore(&pci_lock, f);
    return v;
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v) {
    uint64_t f = spin_lock_irqsave(&pci_lock);
    outl(0xCF8, addr(bus, dev, fn, off));
    outl(0xCFC, v);
    spin_unlock_irqrestore(&pci_lock, f);
}

static void scan_bus(uint8_t bus, int depth);

static void scan_fn(uint8_t bus, uint8_t dev, uint8_t fn, int depth) {
    uint32_t id = pci_read32(bus, dev, fn, 0);
    if ((id & 0xFFFF) == 0xFFFF) return;
    uint32_t cls = pci_read32(bus, dev, fn, 8);
    uint8_t htype = (pci_read32(bus, dev, fn, 0x0C) >> 16) & 0x7F;
    if (ndevs < PCI_MAX_DEVS) {
        struct pci_dev *d = &devs[ndevs++];
        d->bus = bus; d->dev = dev; d->fn = fn;
        d->vendor = id & 0xFFFF; d->device = id >> 16;
        d->class_code = cls >> 24; d->subclass = cls >> 16; d->prog_if = cls >> 8;
        d->irq_line = pci_read32(bus, dev, fn, 0x3C) & 0xFF;
        int nbar = htype == 0 ? 6 : htype == 1 ? 2 : 0;
        for (int i = 0; i < 6; i++) d->bar[i] = i < nbar ? pci_read32(bus, dev, fn, 0x10 + 4 * i) : 0;
    }
    if (htype == 1 && (cls >> 16) == 0x0604 && depth < 8) {   /* PCI-to-PCI bridge */
        uint8_t secondary = (pci_read32(bus, dev, fn, 0x18) >> 8) & 0xFF;
        if (secondary > bus) scan_bus(secondary, depth + 1);
    }
}

static void scan_bus(uint8_t bus, int depth) {
    for (uint8_t dev = 0; dev < 32; dev++) {
        uint32_t id = pci_read32(bus, dev, 0, 0);
        if ((id & 0xFFFF) == 0xFFFF) continue;
        bool multi = (pci_read32(bus, dev, 0, 0x0C) >> 16) & 0x80;
        for (uint8_t fn = 0; fn < (multi ? 8 : 1); fn++) scan_fn(bus, dev, fn, depth);
    }
}

int pci_scan(void) {
    ndevs = 0;
    scan_bus(0, 0);
    for (int i = 0; i < ndevs; i++) {
        const struct pci_dev *d = &devs[i];
        kprintf("pci %02x:%02x.%x %04x:%04x class %02x.%02x.%02x %s irq %u\n",
                d->bus, d->dev, d->fn, d->vendor, d->device, d->class_code, d->subclass,
                d->prog_if, pci_class_name(d->class_code, d->subclass), d->irq_line);
    }
    return ndevs;
}

int pci_count(void) { return ndevs; }
const struct pci_dev *pci_get(int i) { return i >= 0 && i < ndevs ? &devs[i] : 0; }

const struct pci_dev *pci_find_class(uint8_t cls, uint8_t sub, uint8_t prog_if) {
    for (int i = 0; i < ndevs; i++)
        if (devs[i].class_code == cls && devs[i].subclass == sub &&
            (prog_if == 0xFF || devs[i].prog_if == prog_if))
            return &devs[i];
    return 0;
}

const char *pci_class_name(uint8_t cls, uint8_t sub) {
    switch (cls << 8 | sub) {
    case 0x0101: return "ide";
    case 0x0106: return "sata";
    case 0x0100: return "scsi";
    case 0x0200: return "ethernet";
    case 0x0300: return "vga";
    case 0x0380: return "display";
    case 0x0600: return "host-bridge";
    case 0x0601: return "isa-bridge";
    case 0x0604: return "pci-bridge";
    case 0x0C03: return "usb";
    case 0x0C05: return "smbus";
    default: return "other";
    }
}
