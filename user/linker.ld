ENTRY(_start)

PHDRS
{
    rx    PT_LOAD FLAGS(5);       /* PF_R|PF_X */
    rw    PT_LOAD FLAGS(6);       /* PF_R|PF_W */
    stack PT_GNU_STACK FLAGS(6);  /* RW, not X */
}

SECTIONS
{
    . = 0x400000;
    .text : { *(.text .text.*) } : rx
    .rodata : { *(.rodata .rodata.*) } : rx
    . = ALIGN(0x1000);
    .data : { *(.data .data.*) } : rw
    .bss : { *(.bss .bss.*) *(COMMON) } : rw
    .note.GNU-stack : { *(.note.GNU-stack) } : stack
}
