/*
 * syscall / sysret gate. Linux register layout, NTSTATUS in rax.
 *
 * On entry: rax=nr rdi=a0 rsi=a1 rdx=a2 r10=a3 r8=a4 r9=a5
 *           rcx=user rip  r11=user rflags  rsp=user rsp
 *
 * PCB (KERNEL_GS_BASE after swapgs):
 *   +0  kernel_rsp
 *   +8  user_rsp
 *
 * Why this will fail in production:
 *  - No GS cookie. A user who writes KERNEL_GS_BASE (they cannot, CPL0
 *    MSR) is the only GS attack; a bad syscall_init is still fatal.
 *  - sysretq with a non-canonical rcx #GPs; we reject bits 63:47 of rcx.
 */
    .intel_syntax noprefix
    .text
    .extern syscall_from_entry
    .global syscall_entry

syscall_entry:
    swapgs
    mov  gs:[8], rsp
    mov  rsp, gs:[0]
    push gs:[8]              /* user rsp */
    push r11                 /* user rflags */
    push rcx                 /* user rip */
    push r9
    push r8
    push r10
    push rdx
    push rsi
    push rdi
    push rax                 /* nr */
    mov  rdi, rsp            /* frame: nr,a0,a1,a2,a3,a4,a5 */
    cld
    call syscall_from_entry
    add  rsp, 7*8
    pop  rcx                 /* user rip */
    pop  r11                 /* user rflags */
    pop  rsp                 /* user rsp */
    mov  rdx, rcx
    shr  rdx, 47
    test rdx, rdx
    jnz  sysret_bad_rip
    swapgs
    sysretq

sysret_bad_rip:
    swapgs
    ud2

    .section .note.GNU-stack, "", @progbits
