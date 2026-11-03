/* TernOS kernel link layout: higher-half kernel.
 * Virtual (VMA) addresses start at KERNEL_VMA + 1 MiB; GRUB loads the image at the
 * physical (LMA) address 1 MiB. Sections are page-aligned so the VMM can map each
 * with its own permissions (text RX, rodata R, data/bss RW). Keep KERNEL_VMA in sync
 * with kernel/include/mm.h. */
KERNEL_VMA = 0xFFFFFFFF80000000;
/* e_entry is virtual (GRUB requires it inside a segment); the physical entry point
 * comes from the Multiboot2 entry-address tag in boot.S. */
ENTRY(_start)

SECTIONS
{
    . = KERNEL_VMA + 1M;
    __kernel_start = .;

    .text : AT(ADDR(.text) - KERNEL_VMA) ALIGN(4K) {
        __text_start = .;
        KEEP(*(.multiboot2))
        *(.text .text.*)
    }
    . = ALIGN(4K);
    __text_end = .;

    .rodata : AT(ADDR(.rodata) - KERNEL_VMA) ALIGN(4K) {
        __rodata_start = .;
        *(.rodata .rodata.*)
        . = ALIGN(8);
        __extable_start = .;
        KEEP(*(.extable))
        __extable_end = .;
    }
    . = ALIGN(4K);
    __rodata_end = .;

    .data : AT(ADDR(.data) - KERNEL_VMA) ALIGN(4K) {
        __data_start = .;
        *(.data .data.*)
    }
    .bss : AT(ADDR(.bss) - KERNEL_VMA) ALIGN(4K) {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        __bss_end = .;
    }
    . = ALIGN(4K);
    __kernel_end = .;

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
