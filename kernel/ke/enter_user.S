/*
 * Drop to ring 3 via iretq.
 *
 * rdi = user rip
 * rsi = user rsp
 * rdx = user rflags (IF forced on)
 *
 * CS = 0x23 (GDT[4] user 64-bit code | RPL3)
 * SS = 0x1B (GDT[3] user data | RPL3)
 *
 * GS stays 0. syscall_entry swapgs then finds KERNEL_GS_BASE = PCB.
 * Do not swapgs here — that would hand the user the kernel GS.
 */
    .intel_syntax noprefix
    .text
    .global enter_user

enter_user:
    cli
    mov ax, 0x1B
    mov ds, ax
    mov es, ax
    xor r8, r8
    xor r9, r9
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15
    xor rbx, rbx
    xor rbp, rbp
    push 0x1B
    push rsi
    or   rdx, 0x200
    push rdx
    push 0x23
    push rdi
    xor rax, rax
    xor rcx, rcx
    xor rdx, rdx
    xor rdi, rdi
    xor rsi, rsi
    iretq

    .section .note.GNU-stack, "", @progbits
