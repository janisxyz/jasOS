# Syscall ABI

## Gate

`syscall` / `sysret`. Not `int 0x2E`. Not `sysenter`.

MSR setup (`ke/syscall.c`):

- `IA32_EFER.SCE = 1`
- `IA32_STAR`    = `user_cs << 48 | kernel_cs << 32` with the usual
  Intel layout (`kernel_cs`, `kernel_cs+8` data, `user_cs-16` 32-bit
  placeholder, `user_cs-8` user data, `user_cs` user 64-bit code)
- `IA32_LSTAR`   = `syscall_entry`
- `IA32_FMASK`   = `RFLAGS.IF | RFLAGS.DF | RFLAGS.TF | RFLAGS.AC`

`syscall_entry` (GAS): swaps GS to the PCB, stashes user `rsp` in the
thread, loads kernel `rsp0`, builds a trap frame, `sti` is **not** done
until the dispatcher decides. We run syscalls at `PASSIVE_LEVEL` after
the trap frame is safe.

`sysret` restores `rflags` from the frame (so IF returns to whatever
user had), user `rip` must be canonical *and* in user space or we panic
(`sysret` to a kernel RIP is a known Intel footgun).

## Registers

| Arg | Register |
|---|---|
| syscall number | `rax` |
| a0 | `rdi` |
| a1 | `rsi` |
| a2 | `rdx` |
| a3 | `r10`  (**not** `rcx` — `syscall` clobbers `rcx` with `rip`) |
| a4 | `r8` |
| a5 | `r9` |
| return status | `rax` |
| return info | `rdx` (optional, e.g. bytes transferred) |

This is the Linux *layout* with NT *values*. A Windows engineer can
read the status; a gdb user can read the args.

## Status codes (NTSTATUS-shaped)

```
bits 31..30  00 success  01 info  10 warning  11 error
bit  29      customer (we set 0 — this is the OS, not a driver)
bits 16..28  facility (0 = common)
bits 0..15   code
```

`NT_SUCCESS(s)` is `(int32_t)s >= 0`.

| Name | Value | Meaning |
|---|---|---|
| `STATUS_SUCCESS` | `0x00000000` | done |
| `STATUS_TIMEOUT` | `0x00000102` | wait timed out |
| `STATUS_PENDING` | `0x00000103` | IRP queued |
| `STATUS_ABANDONED` | `0x00000080` | mutex owner died |
| `STATUS_UNSUCCESSFUL` | `0xC0000001` | catch-all, avoid |
| `STATUS_NOT_IMPLEMENTED` | `0xC0000002` | number out of range or stub |
| `STATUS_ACCESS_VIOLATION` | `0xC0000005` | bad user pointer |
| `STATUS_INVALID_HANDLE` | `0xC0000008` | hole or wrong type |
| `STATUS_INVALID_PARAMETER` | `0xC000000D` | null where object required |
| `STATUS_NO_SUCH_FILE` | `0xC000000F` | path walk miss |
| `STATUS_END_OF_FILE` | `0xC0000011` | read past end |
| `STATUS_NO_MEMORY` | `0xC0000017` | pmm/heap refused |
| `STATUS_ACCESS_DENIED` | `0xC0000022` | rights mask miss |
| `STATUS_BUFFER_TOO_SMALL` | `0xC0000023` | |
| `STATUS_OBJECT_NAME_NOT_FOUND` | `0xC0000034` | |
| `STATUS_OBJECT_NAME_COLLISION` | `0xC0000035` | |
| `STATUS_INSUFFICIENT_RESOURCES` | `0xC000009A` | |
| `STATUS_FILE_IS_A_DIRECTORY` | `0xC00000BA` | |
| `STATUS_NOT_A_DIRECTORY` | `0xC0000103` | |
| `STATUS_PROCESS_IS_TERMINATING` | `0xC000010A` | |
| `STATUS_CANCELLED` | `0xC0000120` | IRP cancelled |

## Copyin / copyout

Every user pointer is probed:

1. Canonical.
2. `< 0x0000800000000000`.
3. Mapped, user-accessible, and (for write) writable.
4. Length does not wrap.

Failure is `STATUS_ACCESS_VIOLATION`, never a kernel PF. The probe walks
the *current* address space **VADs** (T11), not live `PTE_P` bits.
A committed page that has not yet been touched is a valid copyin target;
`vmm_read_aspace` demand-zeros it. A miss (no VAD, including the user
stack guard hole) is AV. Kernel threads have no user map; a syscall
from a kernel thread with a user pointer is a bug and panics.

Max copyin size v1: 1 MiB. Bigger is `STATUS_INVALID_PARAMETER`.

## Table (v1)

| # | Name | a0 | a1 | a2 | a3 | a4 |
|---|---|---|---|---|---|---|
| 0 | `NtClose` | handle | | | | |
| 1 | `NtDuplicateObject` | src proc | src h | dst proc | access | flags |
| 2 | `NtCreateFile` | out handle | access | path | disp | opts |
| 3 | `NtReadFile` | handle | buf | len | off | out n |
| 4 | `NtWriteFile` | handle | buf | len | off | out n |
| 5 | `NtQueryDirectoryFile` | handle | buf | len | restart | |
| 6 | `NtCreateProcess` | out handle | access | image path | flags | |
| 7 | `NtCreateThread` | out handle | process | entry | arg | |
| 8 | `NtTerminateProcess` | handle (`-1` self) | status | | | |
| 9 | `NtTerminateThread` | handle (`-1` self) | status | | | |
| 10 | `NtWaitForSingleObject` | handle | alertable | timeout_ns | | |
| 11 | `NtCreateEvent` | out handle | access | path | type | init |
| 12 | `NtSetEvent` | handle | | | | |
| 13 | `NtResetEvent` | handle | | | | |
| 14 | `NtCreateMutex` | out handle | access | path | initial owner | |
| 15 | `NtReleaseMutex` | handle | | | | |
| 16 | `NtQuerySystemInformation` | class | buf | len | out | |
| 17 | `NtQueryInformationProcess` | handle | class | buf | len | |
| 18 | `NtYieldExecution` | | | | | |
| 19 | `NtDelayExecution` | alertable | timeout_ns | | | |
| 20 | `NtQueryObject` | handle | class | buf | len | |
| 21 | `NtCreateSection` | out | access | size | prot | file |
| 22 | `NtMapViewOfSection` | section | process | base | size | prot |
| 23 | `NtUnmapViewOfSection` | process | base | | | |
| 24 | `NtAllocateVirtualMemory` | process | base | size | type | prot |
| 25 | `NtFreeVirtualMemory` | process | base | size | type | |
| 26 | `NtQueryVirtualMemory` | process | addr | class | buf | |
| 27 | `NtCreateDirectoryObject` | out | access | path | | |
| 28 | `NtOpenDirectoryObject` | out | access | path | | |
| 29 | `NtRaiseException` | code | | | | |
| 30 | `NtGetCwd` | buf | cap | | | |
| 31 | `NtSetCwd` | path | | | | |
| 32 | `NtCreatePipe` | out read | out write | | | |
| 33 | `NtCreateTimer` | out | name | auto | | |
| 34 | `NtSetTimer` | handle | due ticks | period | | |
| 35 | `NtCancelTimer` | handle | | | | |
| 36 | `NtWaitForMultipleObjects` | handles | count | wait_all | timeout | |
| 37 | `NtProtectVirtualMemory` | process | base | size | prot | old_prot |

`-1` as a process/thread handle means current. Real handles are
`(generation << 16) | (index << 2)`, never all-ones.

T12: `NtDuplicateObject` a3 is the out-handle pointer (syscall bounce),
a4 access, a5 flags: `DUPLICATE_CLOSE_SOURCE` (1), `SAME_ACCESS` (2),
`INHERIT` (4). Foreign src/dst process handles require
`PROCESS_DUP_HANDLE` (0x40). `NtTerminateThread` a0 may be a real
thread handle.

## Marshalling (T6)

`syscall_dispatch` never hands a user pointer to an `Nt*` implementation
when `process->user_mode` is set. Paths go through `copyinstr`. Buffers
bounce through `kalloc` capped at `SYSCALL_COPY_MAX` (64 KiB). Out-handles
go through `copyout`. `NtAllocateVirtualMemory` / `NtMapViewOfSection`
copy the inout `base`. Kernel-linked builtins (`user_mode == 0`) still
pass host pointers; that is the host HAL, not a security boundary.

## Security notes (locked)

- No syscall accepts a kernel pointer from user. If `aN` is a pointer,
  it is a user VA.
- Handles are looked up with the required access; the object pointer
  is referenced for the call and dereferenced on the way out, including
  error paths.
- `NtRaiseException` from user kills the thread, not the kernel.

## Reversal log

none.

T6: handle encoding grew a generation field. Syscall numbers 30–36
are live. User `NtCreateThread` a1 is a user RIP stored on the TCB,
never called in kernel.

T12: `NtTerminateThread` a0 may be a real thread handle (not only
`-1`). `NtDuplicateObject` a5 is flags. `PROCESS_DUP_HANDLE` is
required to duplicate into/out of another process.

T13: syscall 37 `NtProtectVirtualMemory`. Whole VAD only. W^X is
`INVALID_PAGE_PROTECTION`.

T14: `NtProtectVirtualMemory` splits a containing VAD. Subrange is
no longer `NOT_SUPPORTED`. A range that is not contained in one VAD
is `CONFLICTING_ADDRESSES`. `NtFreeVirtualMemory` honours `size`;
`size == 0` releases the whole VAD at `base` (NT `MEM_RELEASE`).
`NtUnmapViewOfSection` passes size 0.

T15: `NtFreeVirtualMemory` / unmap collect frames then `pmm_free` after
dropping VMM. `/bin/echo` is ET_EXEC; `NtCreateProcess("/bin/echo")` is
the ring-3 path. `user_launch` writes `argc=1`, `argv[0]=image`. Extra
operands are T16 — the syscall still has no argv vector.





