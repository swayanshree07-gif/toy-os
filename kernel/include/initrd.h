#ifndef TERN_INITRD_H
#define TERN_INITRD_H
/* Initial RAM disk: a read-only archive loaded by GRUB as a Multiboot2 module
 * (cmdline "initrd"), in TernOS's own "TRD" format built by tools/mkinitrd.py:
 *   header  16 B: "TERNRD1\0", u32 count, u32 total size
 *   entries 64 B: char name[48], u32 offset, u32 size, u32 crc32, u32 flags
 *   data    16-byte aligned file contents
 * The parser checks every offset/size against the image and every file's CRC-32. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "multiboot2.h"

#define INITRD_NAME_MAX 48

struct initrd_file {
    const char *name;
    const uint8_t *data;
    uint32_t size;
};

struct initrd {
    const uint8_t *base;
    uint32_t size, count;
};

enum {
    INITRD_OK = 0, INITRD_ETOOSMALL = -1, INITRD_EMAGIC = -2, INITRD_ESIZE = -3,
    INITRD_EENTRY = -4, INITRD_ENAME = -5, INITRD_ECRC = -6,
};

/* Validate an image in memory; fills *rd on success. */
int initrd_parse(const void *image, size_t len, struct initrd *rd);
bool initrd_get(const struct initrd *rd, uint32_t i, struct initrd_file *out);
bool initrd_find(const struct initrd *rd, const char *name, struct initrd_file *out);

/* The boot initrd: located from the Multiboot2 modules. */
int initrd_init(const struct boot_info *bi);
const struct initrd *initrd_boot(void);    /* NULL if there is none */

uint32_t crc32(const void *data, size_t len);
void test_initrd(void);

#endif
