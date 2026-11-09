#ifndef TERN_PMM_H
#define TERN_PMM_H
/* Physical memory manager: one bit per 4 KiB frame (1 = used), built from the
 * Multiboot2 memory map. Frames below 1 MiB, the kernel image, the boot information,
 * boot modules and the bitmap itself are reserved. */
#include <stdint.h>
#include <stdbool.h>
#include "multiboot2.h"

void pmm_init(const struct boot_info *bi);
uint64_t pmm_alloc(void);              /* physical address of a free frame, 0 if none */
uint64_t pmm_alloc_zeroed(void);       /* same, frame cleared */
void pmm_free(uint64_t pa);            /* panics on double free / bad address */
bool pmm_is_used(uint64_t pa);
uint64_t pmm_free_frames(void);
uint64_t pmm_total_frames(void);       /* frames covered by the bitmap */
/* Day 22: shared frames (copy-on-write). pmm_ref adds an owner; pmm_free drops one and
 * only frees the frame when the last owner lets go. */
void pmm_ref(uint64_t pa);
unsigned pmm_refcount(uint64_t pa);    /* owners, 0 if the frame is free */
uint64_t pmm_shared_frames(void);      /* frames with more than one owner */

/* Reserved ranges recorded at init (for tests and diagnostics). */
struct pmm_range { uint64_t start, end; const char *what; };
unsigned pmm_reserved(const struct pmm_range **out);

#endif
