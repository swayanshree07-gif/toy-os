/* Initrd tests: the boot image from GRUB, plus parser checks on corrupted copies. */
#include "initrd.h"
#include "heap.h"
#include "string.h"
#include "kernel.h"
#include "kprintf.h"
#include "ktest.h"

static const char motd[] = "Welcome to TernOS, a small seabird of an operating system.\n";

void test_initrd(void) {
    const struct initrd *rd = initrd_boot();
    if (!rd) panic("initrd: no boot initrd");
    struct initrd_file f;
    uint32_t bytes = 0;
    for (uint32_t i = 0; initrd_get(rd, i, &f); i++) {
        kprintf("  %-40s %6u\n", f.name, f.size);
        bytes += f.size;
    }
    if (!initrd_find(rd, "/etc/motd", &f) || f.size != sizeof motd - 1 ||
        memcmp(f.data, motd, f.size))
        panic("initrd: /etc/motd missing or wrong");
    if (initrd_find(rd, "etc/nonexistent", &f) || initrd_find(rd, "etc", &f))
        panic("initrd: found a file that does not exist");
    kprintf("[TEST] initrd: %u files, %u bytes of data, /etc/motd ok, crc32 verified\n",
            rd->count, bytes);
}

KTEST(initrd_crc32) {
    KEXPECT(crc32("", 0) == 0);
    KEXPECT(crc32("123456789", 9) == 0xCBF43926u);    /* standard check value */
}

KTEST(initrd_rejects_corruption) {
    const struct initrd *boot = initrd_boot();
    KEXPECT(boot != NULL);
    if (!boot) return;
    uint8_t *copy = kmalloc(boot->size);
    KEXPECT(copy != NULL);
    if (!copy) return;
    struct initrd rd;
    memcpy(copy, boot->base, boot->size);
    KEXPECT(initrd_parse(copy, boot->size, &rd) == INITRD_OK && rd.count == boot->count);
    KEXPECT(initrd_parse(copy, 8, &rd) == INITRD_ETOOSMALL);
    KEXPECT(initrd_parse(copy, boot->size - 1, &rd) == INITRD_ESIZE);   /* truncated */
    copy[0] ^= 1;
    KEXPECT(initrd_parse(copy, boot->size, &rd) == INITRD_EMAGIC);
    copy[0] ^= 1;
    copy[boot->size - 1] ^= 0xFF;          /* last file's data (or its padding) */
    struct initrd_file last;
    initrd_get(boot, boot->count - 1, &last);
    bool in_data = last.data + last.size == boot->base + boot->size;
    KEXPECT(initrd_parse(copy, boot->size, &rd) == (in_data ? INITRD_ECRC : INITRD_OK));
    copy[boot->size - 1] ^= 0xFF;
    uint8_t *e0 = copy + 16;
    e0[52] = 0xFF; e0[53] = 0xFF; e0[54] = 0xFF;   /* first file's size runs off the end */
    KEXPECT(initrd_parse(copy, boot->size, &rd) == INITRD_EENTRY);
    memcpy(copy, boot->base, boot->size);
    memset(e0, 'x', INITRD_NAME_MAX);               /* name without NUL */
    KEXPECT(initrd_parse(copy, boot->size, &rd) == INITRD_ENAME);
    memcpy(copy, boot->base, boot->size);
    e0[48 + 8] ^= 1;                                /* first file's crc */
    KEXPECT(initrd_parse(copy, boot->size, &rd) == INITRD_ECRC);
    kfree(copy);
}
