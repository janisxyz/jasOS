ENTRY(_start)

KERNEL_VMA = 0xFFFFFFFF80000000;
KERNEL_LMA = 0x0000000000100000;

PHDRS
{
    boot PT_LOAD FLAGS(5); /* PF_R|PF_X */
    rx   PT_LOAD FLAGS(5);
    rw   PT_LOAD FLAGS(6); /* PF_R|PF_W */
}

SECTIONS
{
    . = KERNEL_LMA;

    .boot ALIGN(8) : {
        KEEP(*(.multiboot_header))
        *(.text.boot)
        *(.bootbss)
    } : boot

    /* Higher-half: VMA = KERNEL_VMA + current LMA (which is still
       KERNEL_LMA + boot). AT(addr - KERNEL_VMA) then lands physically
       immediately after .boot, not at 0x8000. */
    . = KERNEL_VMA + .;

    _kernel_start = .;


    .text ALIGN(16) : AT(ADDR(.text) - KERNEL_VMA) {
        *(.text .text.*)
    } : rx

    .rodata ALIGN(16) : AT(ADDR(.rodata) - KERNEL_VMA) {
        *(.rodata .rodata.*)
    } : rx

    /* Split RX from RW on a 2 MiB boundary so vmm_init can map
       text+rodata without PTE_W and data+bss with PTE_NX. */
    . = ALIGN(0x200000);
    _rx_end = .;

    .data ALIGN(16) : AT(ADDR(.data) - KERNEL_VMA) {
        *(.data .data.*)
    } : rw

    .bss ALIGN(16) : AT(ADDR(.bss) - KERNEL_VMA) {
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        _bss_end = .;
    } : rw

    _kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame)
        *(.comment)
        *(.note*)
    }
}
