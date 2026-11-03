/* TernOS kernel link layout: identity-mapped at 1 MiB (higher half comes on Day 8). */
ENTRY(_start)

SECTIONS
{
    . = 1M;
    __kernel_start = .;

    .text : ALIGN(4K) {
        KEEP(*(.multiboot2))
        *(.text .text.*)
    }
    .rodata : ALIGN(4K) {
        *(.rodata .rodata.*)
        . = ALIGN(8);
        __extable_start = .;
        KEEP(*(.extable))
        __extable_end = .;
    }
    .data : ALIGN(4K) {
        *(.data .data.*)
    }
    .bss : ALIGN(4K) {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        __bss_end = .;
    }
    __kernel_end = .;

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
