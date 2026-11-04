#ifndef TERN_ACPI_H
#define TERN_ACPI_H
/* Minimal ACPI: find the RSDP (from Multiboot2), walk the RSDT/XSDT, parse the MADT. */
#include <stdint.h>
#include <stdbool.h>
#include "multiboot2.h"

struct acpi_sdt {
    char sig[4];
    uint32_t length;
    uint8_t revision, checksum;
    char oem_id[6], oem_table_id[8];
    uint32_t oem_rev, creator_id, creator_rev;
} __attribute__((packed));

#define MADT_MAX_CPUS    16
#define MADT_MAX_IOAPICS 4

struct madt_info {
    bool valid;
    uint64_t lapic_phys;           /* local APIC MMIO base (after any 64-bit override) */
    bool has_8259;                 /* MADT flags bit 0: dual 8259 PICs present */
    unsigned cpu_count;            /* enabled processor local APICs */
    uint8_t cpu_apic_ids[MADT_MAX_CPUS];
    unsigned ioapic_count;
    struct { uint8_t id; uint32_t phys, gsi_base; } ioapics[MADT_MAX_IOAPICS];
    /* ISA IRQ -> global system interrupt, with polarity/trigger (MPS INTI flags) */
    uint32_t isa_gsi[16];
    uint16_t isa_flags[16];
    unsigned overrides;
};

/* Returns false if there is no valid RSDP / root table. */
bool acpi_init(const struct boot_info *bi);
/* Find the n-th table with the given signature (checksum-verified), or NULL. */
const struct acpi_sdt *acpi_find(const char sig[4], unsigned n);
const struct madt_info *acpi_madt(void);
unsigned acpi_table_count(void);

#endif
