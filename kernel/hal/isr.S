/*
 * Interrupt stubs. Every gate uses an IST (see idt.c) so the CPU
 * always pushes a 5-word iretq frame even when we interrupt kernel
 * mode — the v0.5 path assumed a user frame and triple-faulted on
 * the first PIT tick in idle.
 *
 * Why this will still fail in production:
 *  - One IST for IRQs. Nested IRQ with IF set smashes it. We keep
 *    interrupt gates (IF cleared).
 *  - No CR2 in the hardware frame; C reads CR2 for #PF.
 */
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
    STUB 1, 0
    STUB 2, 0
    STUB 3, 0
    STUB 4, 0
    STUB 5, 0
    STUB 6, 0
    STUB 7, 0
    STUB 8, 1
    STUB 9, 0
    STUB 10, 1
    STUB 11, 1
    STUB 12, 1
    STUB 13, 1
    STUB 14, 1
    STUB 15, 0
    STUB 16, 0
    STUB 17, 1
    STUB 18, 0
    STUB 19, 0
    STUB 20, 0
    STUB 21, 0
    STUB 22, 0
    STUB 23, 0
    STUB 24, 0
    STUB 25, 0
    STUB 26, 0
    STUB 27, 0
    STUB 28, 0
    STUB 29, 0
    STUB 30, 0
    STUB 31, 0
    STUB 32, 0
    STUB 33, 0

    .global isr_stub_generic
isr_stub_generic:
    push 0
    push 0xFF
    jmp isr_common

    .global isr_stub_table
isr_stub_table:
    .quad isr_stub_0,  isr_stub_1,  isr_stub_2,  isr_stub_3
    .quad isr_stub_4,  isr_stub_5,  isr_stub_6,  isr_stub_7
    .quad isr_stub_8,  isr_stub_9,  isr_stub_10, isr_stub_11
    .quad isr_stub_12, isr_stub_13, isr_stub_14, isr_stub_15
    .quad isr_stub_16, isr_stub_17, isr_stub_18, isr_stub_19
    .quad isr_stub_20, isr_stub_21, isr_stub_22, isr_stub_23
    .quad isr_stub_24, isr_stub_25, isr_stub_26, isr_stub_27
    .quad isr_stub_28, isr_stub_29, isr_stub_30, isr_stub_31
    .quad isr_stub_32, isr_stub_33

/*
 * Stack on entry to isr_common (IST):
 *   [ss][rsp][rflags][cs][rip][error][vector]  <- rsp
 * After GPR push, rdi = &trap_frame.
 */
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

    /* swapgs if we came from user (CS.RPL == 3). CS is at
       vector/error + 15 GPRs + rip + cs = 17*8 from rsp. */
    mov  rax, [rsp + 17*8]
    test rax, 3
    jz   1f
    swapgs
1:
    mov  rdi, rsp
    cld
    call isr_dispatch

    mov  rax, [rsp + 17*8]
    test rax, 3
    jz   2f
    swapgs
2:
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
    add rsp, 16              /* vector, error */
    iretq

    .section .note.GNU-stack, "", @progbits
