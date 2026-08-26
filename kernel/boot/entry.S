/* Multiboot2 + 32→64 trampoline. Linked at physical 1 MiB (.boot). */

#define MB2_MAGIC      0xE85250D6
#define MB2_ARCH       0
#define KERNEL_VMA     0xFFFFFFFF80000000

    .section .multiboot_header, "a"
    .align 8
mb2_hdr:
    .long MB2_MAGIC
    .long MB2_ARCH
    .long mb2_end - mb2_hdr
    .long -(MB2_MAGIC + MB2_ARCH + (mb2_end - mb2_hdr))
    /* framebuffer request: 80x25 text */
    .align 8
    .short 5
    .short 0
    .long 20
    .long 80
    .long 25
    .long 0
    .align 8
    .short 0
    .short 0
    .long 8
mb2_end:

    .section .text.boot, "ax"
    .code32
    .global _start
_start:
    cli
    /* stash multiboot: eax magic, ebx info */
    movl %eax, (mb_magic)
    movl %ebx, (mb_info)

    /* page tables at known physical addresses in .bss.boot — we use
       static tables in this file's .bss, which is still in low mem
       because .boot is below the VMA bump. After the VMA bump, bss
       is high. Keep boot tables in .bootbss. */
    movl $boot_pml4, %edi
    movl %edi, %cr3

    /* boot_pml4[0] = boot_pdpt | P|W */
    movl $boot_pdpt, %eax
    orl  $0x03, %eax
    movl %eax, boot_pml4
    movl $0, boot_pml4 + 4

    /* boot_pml4[511] = boot_pdpt (higher-half kernel) */
    movl $boot_pdpt, %eax
    orl  $0x03, %eax
    movl %eax, boot_pml4 + 511 * 8
    movl $0, boot_pml4 + 511 * 8 + 4

    /* boot_pdpt[0] = boot_pd | P|W  (identity 0-1GB) */
    movl $boot_pd, %eax
    orl  $0x03, %eax
    movl %eax, boot_pdpt
    movl $0, boot_pdpt + 4

    /* boot_pdpt[510] = boot_pd for 0xFFFFFFFF80000000
       0xFFFFFFFF80000000: pml4=511, pdpt=510 */
    movl $boot_pd, %eax
    orl  $0x03, %eax
    movl %eax, boot_pdpt + 510 * 8
    movl $0, boot_pdpt + 510 * 8 + 4

    /* 512 x 2MiB identity entries in boot_pd */
    movl $boot_pd, %edi
    movl $0, %eax
    movl $512, %ecx
1:
    movl %eax, %ebx
    orl  $0x83, %ebx          /* P|W|PS */
    movl %ebx, (%edi)
    movl $0, 4(%edi)
    addl $0x200000, %eax
    addl $8, %edi
    loop 1b

    /* GDT */
    lgdt boot_gdt_ptr

    /* PAE */
    movl %cr4, %eax
    orl  $(1 << 5), %eax
    movl %eax, %cr4

    /* EFER.LME */
    movl $0xC0000080, %ecx
    rdmsr
    orl  $(1 << 8), %eax
    wrmsr

    /* PG + PE */
    movl %cr0, %eax
    orl  $0x80000001, %eax
    movl %eax, %cr0

    ljmp $0x08, $long_mode

    .code64
long_mode:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs
    movq $boot_stack_top, %rsp

    xorq %rdi, %rdi
    movl mb_info, %edi
    movabs $kmain_early, %rax
    callq *%rax

1:  hlt
    jmp 1b

    .align 8
mb_magic: .long 0
mb_info:  .long 0

    .align 16
boot_gdt:
    .quad 0
    .quad 0x00AF9A000000FFFF  /* 64-bit code */
    .quad 0x00AF92000000FFFF  /* 64-bit data */
boot_gdt_end:
boot_gdt_ptr:
    .word boot_gdt_end - boot_gdt - 1
    .long boot_gdt

    .section .bootbss, "aw", @nobits
    .align 4096
boot_pml4: .space 4096
boot_pdpt: .space 4096
boot_pd:   .space 4096
    .align 16
boot_stack: .space 16384
boot_stack_top:

    .section .note.GNU-stack, "", @progbits
