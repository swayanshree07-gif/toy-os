#ifndef TERN_MULTIBOOT2_H
#define TERN_MULTIBOOT2_H
#include <stdint.h>

#define MB2_BOOTLOADER_MAGIC 0x36D76289

enum {
    MB2_TAG_END = 0,
    MB2_TAG_CMDLINE = 1,
    MB2_TAG_BOOTLOADER = 2,
    MB2_TAG_MODULE = 3,
    MB2_TAG_BASIC_MEMINFO = 4,
    MB2_TAG_MMAP = 6,
    MB2_TAG_FRAMEBUFFER = 8,
    MB2_TAG_EFI64 = 12,
    MB2_TAG_ACPI_OLD = 14,
    MB2_TAG_ACPI_NEW = 15,
};

struct mb2_tag { uint32_t type, size; };

struct mb2_mmap_entry { uint64_t addr, len; uint32_t type, reserved; };

struct mb2_tag_mmap {
    uint32_t type, size, entry_size, entry_version;
    struct mb2_mmap_entry entries[];
};

struct mb2_tag_framebuffer {
    uint32_t type, size;
    uint64_t addr;
    uint32_t pitch, width, height;
    uint8_t bpp, fb_type;
    uint16_t reserved;
    /* colour info follows for fb_type 1 (RGB) */
    uint8_t red_pos, red_size, green_pos, green_size, blue_pos, blue_size;
} __attribute__((packed));

struct mb2_tag_module { uint32_t type, size, mod_start, mod_end; char cmdline[]; };

#define MB2_MAX_MODULES 8
struct boot_module { uint64_t start, end; const char *cmdline; };   /* physical [start,end) */

/* Parsed summary of everything the kernel cares about. Pointers are virtual (direct map). */
struct boot_info {
    uint64_t info_phys;                 /* the Multiboot2 info block itself (physical) */
    uint32_t info_size;
    const char *cmdline;
    const char *bootloader;
    struct mb2_mmap_entry *mmap;
    uint32_t mmap_count, mmap_entry_size;
    uint64_t usable_bytes;
    struct mb2_tag_framebuffer *fb;
    uint32_t module_count;
    struct boot_module modules[MB2_MAX_MODULES];
    uint32_t tag_count;
};

/* info_phys: physical address passed by the bootloader in EBX. */
int mb2_parse(uint32_t magic, uint64_t info_phys, struct boot_info *out);

#endif
