#ifndef TERN_FBDEV_H
#define TERN_FBDEV_H
/* User-program access to the framebuffer (SYS_fbinfo/fbacquire/fbblit), Day 11 session. */
#include <stdint.h>
#include "syscall_nr.h"

int fbdev_info(struct tern_fbinfo *out);
int fbdev_acquire(int pid);              /* 0, -EBUSY, -ENODEV */
int fbdev_release_by(int pid);           /* 0, or -EPERM if pid isn't the owner */
int fbdev_blit(int pid, const struct tern_fbblit *b, uint64_t pml4);
int fbdev_owner(void);                   /* pid or 0 */
void fbdev_stats(uint64_t *nblits, uint64_t *npixels);

#endif
