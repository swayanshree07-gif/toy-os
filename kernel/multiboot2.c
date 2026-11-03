/* Multiboot2 boot information parser. */
#include "multiboot2.h"
#include "string.h"
#include "mm.h"

int mb2_parse(uint32_t magic, uint64_t info_phys, struct boot_info *out) {
    memset(out, 0, sizeof *out);
    if (magic != MB2_BOOTLOADER_MAGIC || !info_phys || (info_phys & 7))
        return -1;

    uintptr_t info_addr = (uintptr_t)phys_to_virt(info_phys);
    uint32_t total = *(uint32_t *)info_addr;
    out->info_phys = info_phys;
    out->info_size = total;
    uintptr_t p = info_addr + 8, end = info_addr + total;

    while (p + sizeof(struct mb2_tag) <= end) {
        struct mb2_tag *tag = (struct mb2_tag *)p;
        if (tag->type == MB2_TAG_END) break;
        if (tag->size < 8 || p + tag->size > end) return -2;
        out->tag_count++;

        switch (tag->type) {
        case MB2_TAG_CMDLINE:
            out->cmdline = (const char *)(p + 8);
            break;
        case MB2_TAG_BOOTLOADER:
            out->bootloader = (const char *)(p + 8);
            break;
        case MB2_TAG_MODULE: {
            struct mb2_tag_module *m = (struct mb2_tag_module *)tag;
            if (out->module_count < MB2_MAX_MODULES) {
                struct boot_module *bm = &out->modules[out->module_count];
                bm->start = m->mod_start;
                bm->end = m->mod_end;
                bm->cmdline = m->cmdline;
            }
            out->module_count++;
            break;
        }
        case MB2_TAG_MMAP: {
            struct mb2_tag_mmap *m = (struct mb2_tag_mmap *)tag;
            if (m->entry_size < sizeof(struct mb2_mmap_entry)) return -3;
            out->mmap = m->entries;
            out->mmap_entry_size = m->entry_size;
            out->mmap_count = (m->size - 16) / m->entry_size;
            for (uint32_t i = 0; i < out->mmap_count; i++) {
                struct mb2_mmap_entry *e =
                    (struct mb2_mmap_entry *)((uintptr_t)m->entries + i * m->entry_size);
                if (e->type == 1) out->usable_bytes += e->len;
            }
            break;
        }
        case MB2_TAG_FRAMEBUFFER:
            out->fb = (struct mb2_tag_framebuffer *)tag;
            break;
        default:
            break;
        }
        p += (tag->size + 7) & ~7u;
    }
    return 0;
}
