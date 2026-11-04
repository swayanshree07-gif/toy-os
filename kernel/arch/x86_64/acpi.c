/* Minimal ACPI table discovery. All tables are read through the direct map (they live
 * in RAM below the direct-map limit on every machine we target). */
#include "acpi.h"
#include "mm.h"
#include "string.h"
#include "kprintf.h"

struct rsdp {
    char sig[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt;
    /* ACPI 2.0+ */
    uint32_t length;
    uint64_t xsdt;
    uint8_t ext_checksum, reserved[3];
} __attribute__((packed));

static const struct acpi_sdt *root;
static bool root_is_xsdt;
static unsigned n_tables;
static struct madt_info madt;

static bool sum_ok(const void *p, uint32_t len) {
    uint8_t s = 0;
    for (uint32_t i = 0; i < len; i++) s += ((const uint8_t *)p)[i];
    return s == 0;
}

static const struct acpi_sdt *table_at(uint64_t pa) {
    if (!pa) return NULL;
    const struct acpi_sdt *t = phys_to_virt(pa);
    if (t->length < sizeof *t || t->length > (1u << 20) || !sum_ok(t, t->length)) return NULL;
    return t;
}

static uint64_t entry(unsigned i) {
    const uint8_t *base = (const uint8_t *)(root + 1);
    if (root_is_xsdt) { uint64_t v; memcpy(&v, base + 8 * i, 8); return v; }
    uint32_t v; memcpy(&v, base + 4 * i, 4); return v;
}

const struct acpi_sdt *acpi_find(const char sig[4], unsigned n) {
    for (unsigned i = 0; i < n_tables; i++) {
        const struct acpi_sdt *t = table_at(entry(i));
        if (t && !memcmp(t->sig, sig, 4) && n-- == 0) return t;
    }
    return NULL;
}

unsigned acpi_table_count(void) { return n_tables; }
const struct madt_info *acpi_madt(void) { return &madt; }

static void parse_madt(const struct acpi_sdt *t) {
    const uint8_t *p = (const uint8_t *)t + sizeof *t;
    const uint8_t *end = (const uint8_t *)t + t->length;
    uint32_t lapic32, flags;
    memcpy(&lapic32, p, 4);
    memcpy(&flags, p + 4, 4);
    madt.lapic_phys = lapic32;
    madt.has_8259 = flags & 1;
    for (int i = 0; i < 16; i++) { madt.isa_gsi[i] = i; madt.isa_flags[i] = 0; }
    p += 8;
    while (p + 2 <= end && p[1] >= 2 && p + p[1] <= end) {
        uint8_t type = p[0], len = p[1];
        switch (type) {
        case 0:   /* processor local APIC: acpi id, apic id, flags */
            if (len >= 8) {
                uint32_t f; memcpy(&f, p + 4, 4);
                if ((f & 1) && madt.cpu_count < MADT_MAX_CPUS)
                    madt.cpu_apic_ids[madt.cpu_count++] = p[3];
            }
            break;
        case 1:   /* I/O APIC: id, reserved, address, gsi base */
            if (len >= 12 && madt.ioapic_count < MADT_MAX_IOAPICS) {
                unsigned k = madt.ioapic_count++;
                madt.ioapics[k].id = p[2];
                memcpy(&madt.ioapics[k].phys, p + 4, 4);
                memcpy(&madt.ioapics[k].gsi_base, p + 8, 4);
            }
            break;
        case 2:   /* interrupt source override: bus, source, gsi, flags */
            if (len >= 10 && p[2] == 0 && p[3] < 16) {
                memcpy(&madt.isa_gsi[p[3]], p + 4, 4);
                memcpy(&madt.isa_flags[p[3]], p + 8, 2);
                madt.overrides++;
            }
            break;
        case 5:   /* local APIC address override (64-bit) */
            if (len >= 12) memcpy(&madt.lapic_phys, p + 4, 8);
            break;
        default:
            break;
        }
        p += len;
    }
    madt.valid = madt.lapic_phys != 0;
}

bool acpi_init(const struct boot_info *bi) {
    const struct rsdp *r = bi->rsdp;
    if (!r || bi->rsdp_len < 20 || memcmp(r->sig, "RSD PTR ", 8) || !sum_ok(r, 20)) {
        kprintf("acpi: no valid RSDP\n");
        return false;
    }
    if (r->revision >= 2 && bi->rsdp_len >= 36 && sum_ok(r, 36) && r->xsdt) {
        root = table_at(r->xsdt);
        root_is_xsdt = root != NULL;
    }
    if (!root) root = table_at(r->rsdt);
    if (!root || memcmp(root->sig, root_is_xsdt ? "XSDT" : "RSDT", 4)) {
        kprintf("acpi: bad root table\n");
        root = NULL;
        return false;
    }
    n_tables = (root->length - sizeof *root) / (root_is_xsdt ? 8 : 4);
    kprintf("acpi: rev %u %s with %u tables:", r->revision, root_is_xsdt ? "XSDT" : "RSDT", n_tables);
    for (unsigned i = 0; i < n_tables; i++) {
        const struct acpi_sdt *t = table_at(entry(i));
        if (t) kprintf(" %c%c%c%c", t->sig[0], t->sig[1], t->sig[2], t->sig[3]);
        else kprintf(" (bad)");
    }
    kprintf("\n");
    const struct acpi_sdt *m = acpi_find("APIC", 0);
    if (m) parse_madt(m);
    return true;
}
