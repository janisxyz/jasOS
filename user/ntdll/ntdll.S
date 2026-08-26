/*
 * User ntdll. C ABI (rdi rsi rdx rcx r8 r9) → syscall (rax rdi rsi rdx r10 r8 r9).
 * Linked into ET_EXEC user images, never into the kernel.
 */
    .intel_syntax noprefix
    .text

.macro SYSCALL_STUB name nr
    .global \name
\name:
    mov eax, \nr
    mov r10, rcx
    syscall
    ret
.endm

    SYSCALL_STUB NtClose,                    0
    SYSCALL_STUB NtDuplicateObject,          1
    SYSCALL_STUB NtCreateFile,               2
    SYSCALL_STUB NtReadFile,                 3
    SYSCALL_STUB NtWriteFile,                4
    SYSCALL_STUB NtQueryDirectoryFile,       5
    SYSCALL_STUB NtCreateProcess,            6
    SYSCALL_STUB NtCreateThread,             7
    SYSCALL_STUB NtTerminateProcess,         8
    SYSCALL_STUB NtTerminateThread,          9
    SYSCALL_STUB NtWaitForSingleObject,      10
    SYSCALL_STUB NtCreateEvent,              11
    SYSCALL_STUB NtSetEvent,                 12
    SYSCALL_STUB NtResetEvent,               13
    SYSCALL_STUB NtCreateMutex,              14
    SYSCALL_STUB NtReleaseMutex,             15
    SYSCALL_STUB NtQuerySystemInformation,   16
    SYSCALL_STUB NtQueryInformationProcess,  17
    SYSCALL_STUB NtYieldExecution,           18
    SYSCALL_STUB NtDelayExecution,           19
    SYSCALL_STUB NtQueryObject,              20
    SYSCALL_STUB NtCreateSection,            21
    SYSCALL_STUB NtMapViewOfSection,         22
    SYSCALL_STUB NtUnmapViewOfSection,       23
    SYSCALL_STUB NtAllocateVirtualMemory,    24
    SYSCALL_STUB NtFreeVirtualMemory,        25
    SYSCALL_STUB NtQueryVirtualMemory,       26
    SYSCALL_STUB NtCreateDirectoryObject,    27
    SYSCALL_STUB NtOpenDirectoryObject,      28
    SYSCALL_STUB NtRaiseException,           29
    SYSCALL_STUB NtGetCwd,                   30
    SYSCALL_STUB NtSetCwd,                   31
    SYSCALL_STUB NtCreatePipe,               32
    SYSCALL_STUB NtCreateTimer,              33
    SYSCALL_STUB NtSetTimer,                 34
    SYSCALL_STUB NtCancelTimer,              35
    SYSCALL_STUB NtWaitForMultipleObjects,   36
    SYSCALL_STUB NtProtectVirtualMemory,     37
    SYSCALL_STUB NtOpenProcessToken,         38
    SYSCALL_STUB NtQueryInformationToken,    39
    SYSCALL_STUB NtDuplicateToken,           40
    SYSCALL_STUB NtSetInformationToken,      41
    SYSCALL_STUB NtDeleteFile,               42

    .section .note.GNU-stack, "", @progbits
