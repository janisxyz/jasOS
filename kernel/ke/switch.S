    .intel_syntax noprefix
    .text
    .global context_switch
    /* rdi = old, rsi = new
       layout: r15 r14 r13 r12 rbx rbp rip rsp rflags */
context_switch:
    test rdi, rdi
    jz   load_new
    mov  [rdi + 0x00], r15
    mov  [rdi + 0x08], r14
    mov  [rdi + 0x10], r13
    mov  [rdi + 0x18], r12
    mov  [rdi + 0x20], rbx
    mov  [rdi + 0x28], rbp
    pop  rax                 /* return address = rip */
    mov  [rdi + 0x30], rax
    mov  [rdi + 0x38], rsp
    pushfq
    pop  rax
    mov  [rdi + 0x40], rax
load_new:
    mov  r15, [rsi + 0x00]
    mov  r14, [rsi + 0x08]
    mov  r13, [rsi + 0x10]
    mov  r12, [rsi + 0x18]
    mov  rbx, [rsi + 0x20]
    mov  rbp, [rsi + 0x28]
    mov  rsp, [rsi + 0x38]
    mov  rax, [rsi + 0x40]
    push rax
    popfq
    mov  rax, [rsi + 0x30]
    push rax
    ret

    .section .note.GNU-stack, "", @progbits
