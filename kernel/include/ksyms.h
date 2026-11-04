#ifndef TERN_KSYMS_H
#define TERN_KSYMS_H
/* Kernel symbol table (generated at link time by tools/mksyms.py) and backtraces.
 * The kernel is built with frame pointers, so a backtrace follows the rbp chain:
 * [rbp] = caller's rbp, [rbp+8] = return address. Frames are read with probe_read64,
 * so a corrupt chain stops the walk instead of faulting. */
#include <stdint.h>

struct ksym { uint64_t addr; const char *name; };
extern const struct ksym ksyms[];
extern const unsigned ksyms_count;

/* Name of the function containing addr (and the offset into it), or NULL. */
const char *ksym_lookup(uint64_t addr, uint64_t *offset);
/* Address of a symbol by exact name, 0 if unknown. */
uint64_t ksym_addr(const char *name);

#define BT_MAX_FRAMES 24
/* Collect return addresses starting from frame pointer rbp; returns how many. */
int backtrace_collect(uint64_t rbp, uint64_t *out, int max);
/* Print "backtrace:" + one line per frame, starting with rip (if nonzero). */
void backtrace_print(uint64_t rip, uint64_t rbp);

#endif
