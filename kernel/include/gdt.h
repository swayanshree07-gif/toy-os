#ifndef TERN_GDT_H
#define TERN_GDT_H
#include <stdint.h>

/* Layout chosen so that syscall/sysret work: STAR[47:32]=KCODE, STAR[63:48]=0x10
 * gives user SS = 0x18|3 and user CS = 0x20|3. */
#define GDT_KCODE 0x08
#define GDT_KDATA 0x10
#define GDT_UDATA 0x18
#define GDT_UCODE 0x20
#define GDT_TSS   0x28

struct tss {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

void gdt_init(void);
void tss_set_kernel_stack(uint64_t rsp0);
extern struct tss g_tss;

#endif
