/* TernOS user programs: static ELF64 at 4 MiB. Each segment starts on its own page
 * (the kernel's ELF loader requires page-aligned, non-overlapping PT_LOAD segments). */
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5);   /* R X */
    rodata PT_LOAD FLAGS(4);   /* R   */
    data   PT_LOAD FLAGS(6);   /* R W */
}

SECTIONS
{
    . = 0x400000;
    .text : ALIGN(4K) { *(.text._start) *(.text .text.*) } :text
    . = ALIGN(4K);
    .rodata : ALIGN(4K) { *(.rodata .rodata.*) *(.eh_frame*) } :rodata
    . = ALIGN(4K);
    .data : ALIGN(4K) { *(.data .data.*) } :data
    .bss : { *(.bss .bss.*) *(COMMON) } :data
    _end = .;
    /DISCARD/ : { *(.comment) *(.note*) }
}
