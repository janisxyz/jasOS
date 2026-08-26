/* User CRT0. Linked at 0x400000. Calls main, then NtTerminateProcess(-1, eax). */
    .intel_syntax noprefix
    .text
    .global _start
    .extern main
    .extern NtTerminateProcess
_start:
    xor rbp, rbp
    pop rdi                 /* argc */
    mov rsi, rsp            /* argv */
    and rsp, -16
    call main
    mov rdi, -1
    mov rsi, rax
    call NtTerminateProcess
1:  jmp 1b
    .section .note.GNU-stack, "", @progbits
