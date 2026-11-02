/*
 * TernOS boot stub.
 *
 * GRUB (Multiboot2) enters _start in 32-bit protected mode with paging off,
 * EAX = Multiboot2 magic, EBX = physical address of the boot information.
 * We check for long-mode support, build identity-mapped page tables for the
 * first 4 GiB (2 MiB pages), enable PAE + long mode + paging, load a 64-bit
 * GDT and far-jump into 64-bit code, which calls kmain(magic, info).
 */

#define MB2_MAGIC        0xE85250D6
#define MB2_ARCH_I386    0
#define MB2_BOOT_MAGIC   0x36D76289

#define PTE_P   0x1
#define PTE_W   0x2
#define PTE_PS  0x80

    .section .multiboot2, "a"
    .balign 8
mb2_header:
    .long MB2_MAGIC
    .long MB2_ARCH_I386
    .long mb2_header_end - mb2_header
    .long -(MB2_MAGIC + MB2_ARCH_I386 + (mb2_header_end - mb2_header))

    /* Framebuffer request tag (type 5): prefer 1024x768x32, optional. */
    .balign 8
    .short 5
    .short 1                    /* flags: optional */
    .long 20
    .long 1024
    .long 768
    .long 32

    /* End tag */
    .balign 8
    .short 0
    .short 0
    .long 8
mb2_header_end:

    .section .bss
    .balign 4096
boot_pml4:  .skip 4096
boot_pdpt:  .skip 4096
boot_pd:    .skip 4096 * 4      /* 4 page directories -> 4 GiB */
    .balign 16
boot_stack_bottom:
    .skip 16384
boot_stack_top:

    .section .data
saved_magic: .long 0
saved_info:  .long 0

    .balign 16
gdt64:
    .quad 0                     /* null */
    .quad 0x00AF9A000000FFFF    /* 0x08: kernel code, 64-bit (L=1) */
    .quad 0x00CF92000000FFFF    /* 0x10: kernel data */
gdt64_end:
gdt64_ptr:
    .short gdt64_end - gdt64 - 1
    .quad gdt64

    .section .text
    .code32
    .global _start
_start:
    cli
    cld
    movl $boot_stack_top, %esp
    movl %eax, saved_magic
    movl %ebx, saved_info

    /* Multiboot2 magic check */
    cmpl $MB2_BOOT_MAGIC, %eax
    jne .Lno_mb2

    /* CPUID present? (toggle EFLAGS.ID) */
    pushfl
    popl %eax
    movl %eax, %ecx
    xorl $(1 << 21), %eax
    pushl %eax
    popfl
    pushfl
    popl %eax
    pushl %ecx
    popfl
    cmpl %eax, %ecx
    je .Lno_long

    /* Extended CPUID leaf 0x80000001 available, and LM bit set? */
    movl $0x80000000, %eax
    cpuid
    cmpl $0x80000001, %eax
    jb .Lno_long
    movl $0x80000001, %eax
    cpuid
    testl $(1 << 29), %edx
    jz .Lno_long

    /* Page tables: PML4[0] -> PDPT, PDPT[0..3] -> PD[0..3], PDs map 2 MiB pages */
    movl $boot_pdpt, %eax
    orl $(PTE_P | PTE_W), %eax
    movl %eax, boot_pml4

    xorl %ecx, %ecx
1:  movl %ecx, %eax
    shll $12, %eax
    addl $boot_pd, %eax
    orl $(PTE_P | PTE_W), %eax
    movl %eax, boot_pdpt(, %ecx, 8)
    incl %ecx
    cmpl $4, %ecx
    jb 1b

    xorl %ecx, %ecx              /* entry index 0..2047 across the 4 PDs */
2:  movl %ecx, %eax
    shll $21, %eax               /* low 32 bits of physical address */
    orl $(PTE_P | PTE_W | PTE_PS), %eax
    movl %eax, boot_pd(, %ecx, 8)
    movl %ecx, %eax
    shrl $11, %eax               /* high bits: (i << 21) >> 32 */
    movl %eax, boot_pd + 4(, %ecx, 8)
    incl %ecx
    cmpl $2048, %ecx
    jb 2b

    /* CR3 */
    movl $boot_pml4, %eax
    movl %eax, %cr3

    /* CR4.PAE */
    movl %cr4, %eax
    orl $(1 << 5), %eax
    movl %eax, %cr4

    /* EFER.LME */
    movl $0xC0000080, %ecx
    rdmsr
    orl $(1 << 8), %eax
    wrmsr

    /* CR0.PG | CR0.PE | CR0.WP */
    movl %cr0, %eax
    orl $((1 << 31) | (1 << 16) | 1), %eax
    movl %eax, %cr0

    lgdt gdt64_ptr
    ljmp $0x08, $long_mode_entry

.Lno_mb2:
    movl $no_mb2_msg, %esi
    jmp boot_fail32
.Lno_long:
    movl $no_long_msg, %esi
    jmp boot_fail32

/* Print NUL-terminated string at %esi on COM1 (polling) and halt. */
boot_fail32:
    movw $0x3FD, %dx
3:  inb %dx, %al
    testb $0x20, %al
    jz 3b
    lodsb
    testb %al, %al
    jz 4f
    movw $0x3F8, %dx
    outb %al, %dx
    movw $0x3FD, %dx
    jmp 3b
4:  cli
    hlt
    jmp 4b

    .code64
long_mode_entry:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    xorw %ax, %ax
    movw %ax, %fs
    movw %ax, %gs

    movq $boot_stack_top, %rsp
    xorq %rbp, %rbp

    movl saved_magic, %edi
    movl saved_info, %esi
    call kmain
5:  cli
    hlt
    jmp 5b

    .section .rodata
no_mb2_msg:  .asciz "TernOS: not booted by a Multiboot2 loader\r\n"
no_long_msg: .asciz "TernOS: CPU lacks long mode\r\n"
