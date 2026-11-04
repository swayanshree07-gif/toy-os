#ifndef TERN_PIC_H
#define TERN_PIC_H
#include <stdint.h>
#include <stdbool.h>

void pic_init(void);                  /* remap to vectors 32..47, all lines masked */
void pic_disable(void);               /* mask every line incl. the cascade */
void pic_unmask(unsigned irq);
void pic_mask(unsigned irq);
void pic_eoi(unsigned irq);
bool pic_is_spurious(unsigned irq);   /* handles the EOI rules for IRQ 7/15 */
uint64_t pic_spurious_count(void);
uint16_t pic_get_masks(void);        /* slave IMR << 8 | master IMR */

#endif
