    .intel_syntax noprefix
    .text
    .extern isr_dispatch

    .macro STUB vec, has_error
    .global isr_stub_\vec
isr_stub_\vec:
    .if \has_error == 0
        push 0
    .endif
    push \vec
    jmp isr_common
    .endm

    STUB 0, 0
    STUB 8, 1
    STUB 13, 1
    STUB 14, 1
    STUB 32, 0

    .global isr_stub_generic
isr_stub_generic:
    push 0
    push 0xFF
    jmp isr_common

isr_common:
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rdi, [rsp + 15*8]    /* vector: 15 regs pushed, then vector */
    cld
    call isr_dispatch
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
    add rsp, 16
    iretq

    .section .note.GNU-stack, "", @progbits
