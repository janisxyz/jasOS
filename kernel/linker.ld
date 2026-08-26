ENTRY(_start)

KERNEL_VMA = 0xFFFFFFFF80000000;
KERNEL_LMA = 0x0000000000100000;

SECTIONS
{
    . = KERNEL_LMA;

    .boot ALIGN(8) : {
        KEEP(*(.multiboot_header))
        *(.text.boot)
        *(.bootbss)
    }

    . = KERNEL_VMA + .;

    _kernel_start = .;

    .text ALIGN(16) : AT(ADDR(.text) - KERNEL_VMA) {
        *(.text .text.*)
    }

    .rodata ALIGN(16) : AT(ADDR(.rodata) - KERNEL_VMA) {
        *(.rodata .rodata.*)
    }

    .data ALIGN(16) : AT(ADDR(.data) - KERNEL_VMA) {
        *(.data .data.*)
    }

    .bss ALIGN(16) : AT(ADDR(.bss) - KERNEL_VMA) {
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        _bss_end = .;
    }

    _kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame)
        *(.comment)
        *(.note*)
    }
}
