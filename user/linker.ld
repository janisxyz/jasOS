ENTRY(_start)

PHDRS
{
    rx PT_LOAD FLAGS(5);
    rw PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x400000;
    .text : { *(.text .text.*) } : rx
    .rodata : { *(.rodata .rodata.*) } : rx
    . = ALIGN(0x1000);
    .data : { *(.data .data.*) } : rw
    .bss : { *(.bss .bss.*) *(COMMON) } : rw
}
