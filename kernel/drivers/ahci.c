/* AHCI SATA driver (Day 14 session = re-plan session 14). Polled, one command slot.
 * The HBA registers (ABAR, PCI BAR5) are mapped uncached. Each used port gets one
 * physical frame: command list (1 KiB, slot 0 only) at +0, received-FIS area (256 B)
 * at +0x400, command table (header + 1 PRDT entry) at +0x500. Data goes through a
 * per-port bounce frame, so callers can pass any kernel buffer (heap, stack, ...). */
#include "ahci.h"
#include "pci.h"
#include "vmm.h"
#include "pmm.h"
#include "mm.h"
#include "string.h"
#include "kprintf.h"
#include "sync.h"
#include "syscall_nr.h"

/* HBA (generic) registers */
#define HBA_CAP  0x00
#define HBA_GHC  0x04
#define HBA_PI   0x0C
#define HBA_VS   0x10
#define GHC_AE   (1u << 31)
/* port registers, at 0x100 + port * 0x80 */
#define P_CLB  0x00
#define P_CLBU 0x04
#define P_FB   0x08
#define P_FBU  0x0C
#define P_IS   0x10
#define P_IE   0x14
#define P_CMD  0x18
#define P_TFD  0x20
#define P_SIG  0x24
#define P_SSTS 0x28
#define P_SERR 0x30
#define P_CI   0x38
#define CMD_ST  (1u << 0)
#define CMD_FRE (1u << 4)
#define CMD_FR  (1u << 14)
#define CMD_CR  (1u << 15)
#define TFD_ERR 0x01
#define TFD_DRQ 0x08
#define TFD_BSY 0x80
#define IS_TFES (1u << 30)
#define SIG_ATA   0x00000101u
#define SIG_ATAPI 0xEB140101u

#define ATA_READ_DMA_EXT 0x25
#define ATA_WRITE_DMA_EXT 0x35
#define ATA_FLUSH_CACHE_EXT 0xEA
#define ATA_IDENTIFY     0xEC
#define MAX_DISKS 6
#define SPIN_LIMIT 50000000u

struct port_mem { uint64_t pa; uint8_t *va; uint64_t bounce_pa; uint8_t *bounce; };

static volatile uint8_t *abar;
static struct ahci_disk disks[MAX_DISKS];
static struct port_mem pmem[MAX_DISKS];
static int ndisks;
static struct spinlock ahci_lock = SPINLOCK_INIT("ahci");

static uint32_t rd(uint32_t off) { return *(volatile uint32_t *)(abar + off); }
static void wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(abar + off) = v; }
static uint32_t prd(int p, uint32_t off) { return rd(0x100 + p * 0x80 + off); }
static void pwr(int p, uint32_t off, uint32_t v) { wr(0x100 + p * 0x80 + off, v); }

/* Wait until (reg & mask) == want; false on timeout. */
static bool pwait(int p, uint32_t off, uint32_t mask, uint32_t want) {
    for (uint32_t i = 0; i < SPIN_LIMIT; i++) {
        if ((prd(p, off) & mask) == want) return true;
        __asm__ volatile("pause");
    }
    return false;
}

static bool port_stop(int p) {
    pwr(p, P_CMD, prd(p, P_CMD) & ~CMD_ST);
    if (!pwait(p, P_CMD, CMD_CR, 0)) return false;
    pwr(p, P_CMD, prd(p, P_CMD) & ~CMD_FRE);
    return pwait(p, P_CMD, CMD_FR, 0);
}

static bool port_start(int p, uint64_t mem_pa) {
    pwr(p, P_CLB, (uint32_t)mem_pa);
    pwr(p, P_CLBU, (uint32_t)(mem_pa >> 32));
    pwr(p, P_FB, (uint32_t)(mem_pa + 0x400));
    pwr(p, P_FBU, (uint32_t)((mem_pa + 0x400) >> 32));
    pwr(p, P_SERR, 0xFFFFFFFFu);
    pwr(p, P_IS, 0xFFFFFFFFu);
    pwr(p, P_IE, 0);
    pwr(p, P_CMD, prd(p, P_CMD) | CMD_FRE);
    if (!pwait(p, P_TFD, TFD_BSY | TFD_DRQ, 0)) return false;
    pwr(p, P_CMD, prd(p, P_CMD) | CMD_ST);
    return true;
}

/* Issue one ATA command in slot 0 with one PRDT entry (bytes <= 4096) into the bounce
 * frame; polls for completion. */
static int issue(int d, uint8_t cmd, uint64_t lba, unsigned count, uint32_t bytes) {
    int p = disks[d].port;
    struct port_mem *m = &pmem[d];
    if (!pwait(p, P_TFD, TFD_BSY | TFD_DRQ, 0)) return -EIO;
    uint32_t *hdr = (uint32_t *)m->va;                 /* command header, slot 0 */
    memset(hdr, 0, 32);
    bool write = cmd == ATA_WRITE_DMA_EXT;
    /* CFL = 5 dwords, PRDTL = 1 (0 for non-data commands), W bit for writes */
    hdr[0] = 5 | (write ? 1u << 6 : 0) | (bytes ? 1u << 16 : 0);
    uint64_t ct_pa = m->pa + 0x500;
    hdr[2] = (uint32_t)ct_pa;
    hdr[3] = (uint32_t)(ct_pa >> 32);
    uint8_t *ct = m->va + 0x500;
    memset(ct, 0, 0x80 + 16);
    uint8_t *fis = ct;                                 /* H2D register FIS */
    fis[0] = 0x27; fis[1] = 0x80; fis[2] = cmd;
    fis[4] = lba; fis[5] = lba >> 8; fis[6] = lba >> 16;
    fis[7] = cmd == ATA_IDENTIFY || cmd == ATA_FLUSH_CACHE_EXT ? 0 : 0x40;           /* LBA mode */
    fis[8] = lba >> 24; fis[9] = lba >> 32; fis[10] = lba >> 40;
    fis[12] = count; fis[13] = count >> 8;
    uint32_t *prdt = (uint32_t *)(ct + 0x80);
    prdt[0] = (uint32_t)m->bounce_pa;
    prdt[1] = (uint32_t)(m->bounce_pa >> 32);
    prdt[3] = bytes ? bytes - 1 : 0;
    pwr(p, P_IS, 0xFFFFFFFFu);
    __asm__ volatile("mfence" ::: "memory");
    pwr(p, P_CI, 1);
    for (uint32_t i = 0;; i++) {
        if (prd(p, P_IS) & IS_TFES) break;
        if (!(prd(p, P_CI) & 1)) break;
        if (i == SPIN_LIMIT) return -EIO;
        __asm__ volatile("pause");
    }
    if ((prd(p, P_IS) & IS_TFES) || (prd(p, P_TFD) & TFD_ERR)) {
        /* recover: restart the port so the next command works */
        uint64_t pa = m->pa;
        port_stop(p);
        pwr(p, P_SERR, 0xFFFFFFFFu);
        port_start(p, pa);
        return -EIO;
    }
    return 0;
}

static void ata_string(char *dst, const uint16_t *w, int words) {
    for (int i = 0; i < words; i++) { dst[2 * i] = w[i] >> 8; dst[2 * i + 1] = w[i] & 0xFF; }
    int n = words * 2;
    dst[n] = 0;
    while (n > 0 && dst[n - 1] == ' ') dst[--n] = 0;
}

int ahci_init(void) {
    ndisks = 0;
    const struct pci_dev *pd = pci_find_class(0x01, 0x06, 0x01);
    if (!pd || (pd->bar[5] & 1)) { kprintf("ahci: no controller\n"); return 0; }
    /* memory space + bus master */
    uint32_t cmdreg = pci_read32(pd->bus, pd->dev, pd->fn, 0x04);
    pci_write32(pd->bus, pd->dev, pd->fn, 0x04, (cmdreg & 0xFFFF) | 0x6);
    abar = vmm_map_mmio(pd->bar[5] & ~0xFu, 0x1100);
    if (!abar) { kprintf("ahci: cannot map ABAR\n"); return 0; }
    wr(HBA_GHC, rd(HBA_GHC) | GHC_AE);
    uint32_t pi = rd(HBA_PI), vs = rd(HBA_VS);
    if (!(rd(HBA_CAP) & (1u << 31))) kprintf("ahci: warning, no 64-bit DMA\n");
    kprintf("ahci: version %x.%x, ports implemented %x\n", vs >> 16, vs & 0xFFFF, pi);
    for (int p = 0; p < 32 && ndisks < MAX_DISKS; p++) {
        if (!(pi & (1u << p))) continue;
        uint32_t ssts = prd(p, P_SSTS);
        if ((ssts & 0xF) != 3) continue;               /* no device / no PHY */
        uint32_t sig = prd(p, P_SIG);
        if (sig != SIG_ATA) {
            kprintf("ahci: port %d: %s, skipped\n", p, sig == SIG_ATAPI ? "atapi" : "unknown");
            continue;
        }
        if (!port_stop(p)) { kprintf("ahci: port %d: won't stop\n", p); continue; }
        struct port_mem *m = &pmem[ndisks];
        m->pa = pmm_alloc_zeroed();
        m->bounce_pa = pmm_alloc_zeroed();
        if (!m->pa || !m->bounce_pa) { kprintf("ahci: out of memory\n"); break; }
        m->va = phys_to_virt(m->pa);
        m->bounce = phys_to_virt(m->bounce_pa);
        if (!port_start(p, m->pa)) { kprintf("ahci: port %d: won't start\n", p); continue; }
        struct ahci_disk *d = &disks[ndisks];
        d->port = p;
        if (issue(ndisks, ATA_IDENTIFY, 0, 0, 512)) { kprintf("ahci: port %d: identify failed\n", p); continue; }
        const uint16_t *id = (const uint16_t *)m->bounce;
        d->sectors = (id[83] & (1u << 10))
            ? (uint64_t)id[100] | (uint64_t)id[101] << 16 | (uint64_t)id[102] << 32 | (uint64_t)id[103] << 48
            : (uint64_t)id[60] | (uint64_t)id[61] << 16;
        ata_string(d->model, id + 27, 20);
        kprintf("ahci: disk %d on port %d: \"%s\", %u sectors (%u KiB)\n", ndisks, p, d->model,
                (unsigned)d->sectors, (unsigned)(d->sectors / 2));
        ndisks++;
    }
    return ndisks;
}

int ahci_disk_count(void) { return ndisks; }
const struct ahci_disk *ahci_disk_get(int i) { return i >= 0 && i < ndisks ? &disks[i] : 0; }

int ahci_read(int d, uint64_t lba, unsigned count, void *buf) {
    if (d < 0 || d >= ndisks || count == 0 || count > 8 || lba + count > disks[d].sectors)
        return -EINVAL;
    uint64_t f = spin_lock_irqsave(&ahci_lock);
    int r = issue(d, ATA_READ_DMA_EXT, lba, count, count * 512);
    if (!r) memcpy(buf, pmem[d].bounce, count * 512);
    spin_unlock_irqrestore(&ahci_lock, f);
    return r;
}

int ahci_write(int d, uint64_t lba, unsigned count, const void *buf) {
    if (d < 0 || d >= ndisks || count == 0 || count > 8 || lba + count > disks[d].sectors)
        return -EINVAL;
    uint64_t f = spin_lock_irqsave(&ahci_lock);
    memcpy(pmem[d].bounce, buf, count * 512);
    int r = issue(d, ATA_WRITE_DMA_EXT, lba, count, count * 512);
    spin_unlock_irqrestore(&ahci_lock, f);
    return r;
}

int ahci_flush(int d) {
    if (d < 0 || d >= ndisks) return -EINVAL;
    uint64_t f = spin_lock_irqsave(&ahci_lock);
    int r = issue(d, ATA_FLUSH_CACHE_EXT, 0, 0, 0);
    spin_unlock_irqrestore(&ahci_lock, f);
    return r;
}
