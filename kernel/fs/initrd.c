/* Initrd ("TRD" archive) parser. */
#include "initrd.h"
#include "mm.h"
#include "string.h"
#include "kernel.h"
#include "kprintf.h"

#define HDR_SIZE   16
#define ENTRY_SIZE 64

static struct initrd boot_rd;
static bool have_boot_rd;

uint32_t crc32(const void *data, size_t len) {
    const uint8_t *p = data;
    uint32_t c = 0xFFFFFFFFu;
    while (len--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & -(c & 1));
    }
    return ~c;
}

static uint32_t rd32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int initrd_parse(const void *image, size_t len, struct initrd *rd) {
    const uint8_t *b = image;
    if (len < HDR_SIZE) return INITRD_ETOOSMALL;
    if (memcmp(b, "TERNRD1\0", 8)) return INITRD_EMAGIC;
    uint32_t count = rd32(b + 8), total = rd32(b + 12);
    if (total > len || total < HDR_SIZE || count > (total - HDR_SIZE) / ENTRY_SIZE)
        return INITRD_ESIZE;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = b + HDR_SIZE + i * ENTRY_SIZE;
        uint32_t off = rd32(e + 48), size = rd32(e + 52);
        if (off < HDR_SIZE + count * ENTRY_SIZE || off > total || size > total - off)
            return INITRD_EENTRY;
        if (e[0] == 0 || memchr(e, 0, INITRD_NAME_MAX) == NULL) return INITRD_ENAME;
        if (crc32(b + off, size) != rd32(e + 56)) return INITRD_ECRC;
    }
    rd->base = b;
    rd->size = total;
    rd->count = count;
    return INITRD_OK;
}

bool initrd_get(const struct initrd *rd, uint32_t i, struct initrd_file *out) {
    if (!rd || i >= rd->count) return false;
    const uint8_t *e = rd->base + HDR_SIZE + i * ENTRY_SIZE;
    out->name = (const char *)e;
    out->data = rd->base + rd32(e + 48);
    out->size = rd32(e + 52);
    return true;
}

bool initrd_find(const struct initrd *rd, const char *name, struct initrd_file *out) {
    if (name[0] == '/') name++;
    for (uint32_t i = 0; rd && i < rd->count; i++)
        if (initrd_get(rd, i, out) && strcmp(out->name, name) == 0) return true;
    return false;
}

int initrd_init(const struct boot_info *bi) {
    for (uint32_t i = 0; i < bi->module_count && i < MB2_MAX_MODULES; i++) {
        const struct boot_module *m = &bi->modules[i];
        if (!m->cmdline || !strstr(m->cmdline, "initrd")) continue;
        int rc = initrd_parse(phys_to_virt(m->start), m->end - m->start, &boot_rd);
        if (rc) {
            kprintf("initrd: module %u invalid (error %d)\n", i, rc);
            return rc;
        }
        have_boot_rd = true;
        kprintf("initrd: %u files, %u bytes at phys %lx\n", boot_rd.count, boot_rd.size,
                m->start);
        return 0;
    }
    kprintf("initrd: no module\n");
    return INITRD_ETOOSMALL;
}

const struct initrd *initrd_boot(void) { return have_boot_rd ? &boot_rd : NULL; }
