# jasOS / Aegis — Architecture (locked)

Living document. Append decisions and reversals; do not rewrite history.

**OS name:** jasOS  
**Kernel name:** Aegis  
**License:** MIT  
**Target:** x86_64, higher-half hybrid kernel  
**Boot:** Multiboot2 (QEMU `-kernel` or GRUB2)  
**Primary host for development:** Windows (WSL2 / QEMU / Hyper-V)

---

## 0. Problem statement

Build a bootable x86_64 operating system whose kernel a Windows systems
engineer would recognize: objects, handles, waitable dispatcher objects,
NTSTATUS-shaped returns, a real syscall surface, and a panic path that
does not lie.

This is not a UNIX clone with extra steps. Process, thread, section, file,
device, event, mutex, timer, and directory are first-class kernel objects
with a handle table and an access mask. Userland never holds a kernel
pointer.

## Non-goals (v1)

- Not Linux. No VFS superblock zoo, no cgroups, no POSIX-as-religion.
- Not a desktop. No GPU, no compositor, no Wi-Fi stack.
- Not a hypervisor. No EPT, no nested virt.
- Not a network OS. No TCP/IP in v1 (loopback later).
- Not SMP-complete. One CPU is real; IPI/affinity is stubbed with comments.
- Not a verified kernel. We are hostile-reviewing C, not writing seL4.

## Threat model

| Actor | What they can try | v1 posture |
|---|---|---|
| Malicious userland | Arbitrary syscall args, bad pointers, handle reuse, spray | copyin/copyout, handle-rights, canonical checks, no kernel VA in maps |
| Buggy driver | Bad IRP completion, use-after-free on device object | IRP refcount, cancel routine, driver isolation is *logical* not hardware |
| Bad hardware | Corrupt mmap, spurious IRQ, triple-fault | Panic dumps registers; we do not "recover" from DF |
| Hostile admin | Root-equivalent process | Token object exists; we do not pretend admin is untrusted in v1 |

Isolation is handle-based, not "user is nice". A process that holds a
handle with `FILE_READ_DATA` cannot write. A process that does not hold
a process handle cannot `NtTerminateProcess` it, except self.

---

## 1. Hybrid kernel

Kernel mode owns: HAL, PMM/VMM/heap, object manager, scheduler, wait,
IRP/driver model, VFS, syscall gate.

User mode owns: init, shell, utilities, future daemons.

Drivers in v1 are kernel-resident. They are not a free-for-all: they
speak IRPs, they do not call `pmm_alloc` from a DPC without a documented
lock rank, and they never touch another process's address space except
through `MmCopyVirtualMemory`.

Reversal log: none yet. Limine was considered and rejected — Multiboot2
is enough and does not add a third-party boot protocol to the TCB.

---

## 2. Address space (canonical higher-half)

```
0x0000_0000_0040_0000   user image (ET_EXEC default)
0x0000_0000_0100_0000   user heap (brk/NtAllocateVirtualMemory)
0x0000_7FFF_FF00_0000   user stack (grows down, guard page)
0x0000_7FFF_FFFF_FFFF   user canonical top

0x0000_8000_0000_0000   NON-CANONICAL HOLE
        ...
0xFFFF_7FFF_FFFF_FFFF   NON-CANONICAL HOLE

0xFFFF_8000_0000_0000   direct map of physical memory (HHDM)
0xFFFF_9000_0000_0000   kernel heap (slabs sit here)
0xFFFF_A000_0000_0000   kernel stacks (32 KiB stride, guard page)
0xFFFF_FF00_0000_0000   recursive PML4 window (slot 510)
0xFFFF_FFFF_8000_0000   kernel image (mcmodel=kernel, -2GB)
```

PML4[510] is recursive. Slot 511 is the kernel image. Slot 256 starts
the HHDM. User owns 0..255.

T8: `KERNEL_STACK_BASE` (PML4 320) is pre-created in `vmm_init` so
every user CR3 shares the PDPT. Heap slabs actually live in the HHDM
today (`HHDM_BASE + pa`); `HEAP_BASE` is reserved, not yet the slab
window. Do not put executable leaves in the HHDM.


Invariant: a user page table never contains a mapping with the
supervisor bit clear *and* a kernel virtual address. `NtMapViewOfSection`
rejects any `BaseAddress` that is non-canonical or ≥ `0x0000800000000000`.

---

## 3. Interrupt model

- PIC (8259) + PIT (8253/8254) in v1. Why not LAPIC first: QEMU `-kernel`
  brings up a usable 8259/PIT without MADT parsing. LAPIC timer is the
  next HAL commit, not a v1 blocker.
- IDT: 256 gates. IST1 = double fault, IST2 = NMI, IST3 = machine check.
- IRQ0 (PIT) → `ke_timer_isr` → quantum accounting → possible preemption.
- Exceptions 0, 8, 13, 14 are fatal to the offending thread if user,
  panic if kernel. PF in user with a valid VAD is a demand-zero (v1:
  committed pages only, no real file-backed demand paging yet).
- `cli` is not a lock. Spinlocks raise IRQL (logical) on SMP-future;
  on UP they `cli` and keep a nesting count.

---

## 4. Process model

A **Process** is an object: address space, handle table, token, list of
threads, exit status, job-less in v1.

A **Thread** is an object: kernel stack, user context, wait state,
priority, quantum, TEB-ish user pointer (unused in v1).

There is no "current process" independent of "current thread".
`KeGetCurrentProcess()` is `KeGetCurrentThread()->process`.

init (pid 1) is created by the kernel after the object manager is up.
It is not special-cased in the scheduler; it is special-cased in
reaping: if pid 1 exits, we panic. That is a policy, not a bug.

---

## 5. Syscall style

`syscall`/`sysret` gate. Linux register layout (so gdb muscle memory
works), NT-shaped returns.

See [SYSCALL_ABI.md](SYSCALL_ABI.md).

---

## 6. Boot path

See [BOOT_CONTRACT.md](BOOT_CONTRACT.md).

firmware → Multiboot2 loader (GRUB or QEMU) → 32-bit stub → long mode
+ higher half → GDT/IDT/TSS → PMM from mmap → VMM + heap → object
manager → scheduler + idle → PIT → VFS + ramfs → init → idle loop.

---

## 7. Kernel object types v1

See [OBJECT_MODEL.md](OBJECT_MODEL.md).

`Process Thread Section File Device Event Mutex Timer Directory`

Token is allocated as a field of Process in v1, not a standalone type
yet. Reversal would be to promote it when we have more than two
integrity levels.

---

## 8. Toolchain (locked)

- C11, GCC *or* Clang, freestanding.
- GAS (AT&T) for boot/entry/switch/isr. No NASM dependency: `gcc` drives `as`.
- `ld` with `kernel/linker.ld`.
- Host test target (`make host`) compiles the same mm/ob/ke/fs sources
  with `-DJASOS_HOST` against a POSIX HAL so the object model, VFS and
  scheduler can be executed without QEMU.

Windows: WSL2 Ubuntu 22.04+ with `build-essential qemu-system-x86`.
See [BUILD.md](BUILD.md).

---

## Decision log

| When | Decision | Why |
|---|---|---|
| T0 | Hybrid, not microkernel | Driver isolation is a later pass; objects+handles are the identity |
| T0 | Multiboot2, not UEFI-first | QEMU `-kernel` is the inner loop; UEFI is a loader we can add |
| T0 | PIT+PIC, not LAPIC-first | Less firmware surface to get a ticking quantum |
| T0 | GAS, not NASM | One less tool on Windows/WSL |
| T0 | NTSTATUS, not errno | The audience is a Windows systems engineer |
| T1 | Drop heap/VFS locks before PMM/kalloc | Rank inversion panics were correct; the call graph was wrong |
| T2 | Handle value is `index<<2`, not `(index<<2)\|4` | The tag bit sat in the index field; two handles in one table collided |
| T4 | Host threads switch onto their kstack | Shared boot-stack trampoline smashed wait frames (ping/pong) |
| T4 | `pmm_enter_hhdm` after CR3 load | Frame metadata was identity-mapped; post-vmm accesses would #PF |
| T4 | ELF load copies through `vmm_write_aspace` | memcpy to user VA from the parent CR3 is a #PF |
| T4 | Scheduler loads CR3 + TSS.RSP0 on switch | User processes otherwise ran in the kernel aspace |
| T6 | IST4 for every IRQ/exc | Kernel PIT tick is same-CPL; without IST the CPU omits ss/rsp and the frame lies |
| T6 | Handle = `(gen<<16)|(index<<2)` | Slot reuse after close accepted a stale handle; generation is the lock |
| T6 | Hardware `copyin` walks PTEs, copies via HHDM | STAC+user-VA memcpy is a SMAP hole waiting to happen; HHDM never needs STAC |
| T6 | User `NtCreateThread` parks RIP on the TCB | Calling a user VA from `thread_trampoline` is ring-0 user code. SMEP would #PF it; we refuse to try |
| T6 | Last-thread exit drops aspace + handle table | Leaked user frames and named objects. Rank: drop PROC before VMM/HANDLE |
| T7 | Pipe `open_fn` on insert/dup | Duplicate write handle no longer spuriously EOFs the reader |
| T7 | Syscall Read/Write chunk at 64 KiB | A 1 MiB bounce was a kernel-heap DoS from user |
| T10 | Ramdisk0 is a real IRP disk | dmesg "Ramdisk" with no dispatch was a lie; 1 MiB backing, `/dev/ram0` |
| T10 | virtio-blk identify-only | Probe 0x1AF4/0x1001/0x1042; do not claim virtqueues this pass |
| T10 | IST `.bss` arrays deleted | Guarded `IST_STACK_BASE` is the only landing pad; cli until `tss_map_ist` |
| T11 | `NtAllocateVirtualMemory` is demand-zero | Pre-backing every page made the PF handler dead and a mapping bomb hit PMM |
| T11 | `vmm_probe_user` walks VADs, not `PTE_P` | copyin of a committed untouched page must populate, not AV |
| T12 | Mutex owner death abandons waiters | A dead owner left waiters sleeping forever; owned-mutex list + `STATUS_ABANDONED` |
| T12 | Waiter donates priority to mutex owner | `wait_boost` was a field that did nothing; `sched_boost` requeues READY |
| T12 | `NtTerminateThread` by handle | Self-only was a lie; `kill_pending` on switch-in / trampoline |
| T12 | Handle inherit on `NtCreateProcess` | `slot.inherit` existed and was always 0 |
| T12 | Host shadow is per-page | Whole-VAD `kalloc` was a mapping bomb the commit cap did not stop on host |
| T12 | `ht_destroy` drops HANDLE before `kfree` | Last-thread exit paniced "heap while holding 7" |
| T13 | `g_procs` holds a ref | `NtClose` of a process handle kfree'd the object while the table still walked it (0xAA poison pid) |
| T13 | `NtProtectVirtualMemory` | Whole-VAD prot change; W^X refused; NOACCESS drops user probe; subrange split residual |
| T14 | VAD split on protect/free | Exact-match protect was a lie; `vmm_free_user` ignored size and nuked the whole region |
| T14 | Coalesce after protect | Restore-middle left three RW VADs; the original range could not be named |
| T14 | WAIT_ALL owned mutex | `signal_state` is 0 while held; poll WAIT_ALL timed out on a mutex the caller owned |
| T14 | NOACCESS frame restore | T13 parked the frame in a proto PTE then leaked it on the next RW protect |

---

## T14 surface (0.14.0)

- `NtProtectVirtualMemory` / `vmm_free_user` split a containing VAD: prefix, suffix, or middle. Range must sit in one VAD (holes and multi-VAD spans are `CONFLICTING_ADDRESSES`).
- `size == 0` on free is whole-VAD at `base` (NT `MEM_RELEASE`). `NtUnmapViewOfSection` uses it.
- Adjacent same-prot VADs coalesce after protect so a split-then-restore is nameable as one range again.
- Hardware `apply_prot_range` restores a NOACCESS'd frame (`pa != 0`, `PTE_P` clear) instead of leaking it on the next populate.
- WAIT_ALL: a mutex the caller already owns counts as satisfied.

Residual: protect still will not walk a run of mixed-prot VADs in one call; coalesce is same-prot adjacent only. Unmap still takes PMM while holding VMM.

---

## T13 surface (0.13.0)

- `psp_create_process` takes a table ref on `g_procs`. Reap drops it. Close of the last handle no longer frees a live table entry.
- Syscall 37 `NtProtectVirtualMemory`: exact-VAD only. `PAGE_EXECUTE_READWRITE` is `INVALID_PAGE_PROTECTION`. `PAGE_NOACCESS` fails `vmm_probe_user`. Hardware present pages are `invlpg`'d; NOACCESS clears `PTE_P` and keeps the frame in the PTE.
- Residual: subrange protect does not split VADs (`NOT_SUPPORTED`).

---

---

## T12 surface (0.12.0)

- Mutex rundown: each thread has `owned_mutexes`. Acquire inserts, last-recursion release removes, thread death abandons and wakes with `STATUS_ABANDONED`.
- Priority inheritance: a higher-priority mutex waiter calls `sched_boost` on the owner (DISP held, SCHED is legal). Boost unwinds when the owner holds no more mutexes.
- Process and thread objects are waitable and are actually waited on in host selftest.
- `NtTerminateThread` looks up `THREAD_TERMINATE`, sets `kill_pending`, unlinks waiters, exits on switch-in. Idle is not killable.
- `NtCreateProcess` copies inherit-marked handles. `NtDuplicateObject` takes `DUPLICATE_CLOSE_SOURCE | SAME_ACCESS | INHERIT` and holds `PROCESS_DUP_HANDLE` refs until the copy finishes.
- Host VAD shadow is a pointer array plus one 4 KiB page on first touch, not a whole-VAD slab.
- Host dispatcher restart: `sched_start` rebuilds idle's ucontext; last non-idle exit `longjmp`s without `setcontext` of a stale idle frame.

---

## T11 surface (0.11.0)

- Committed VADs install `PTE_SW_COMMIT` (not-present). First PF / `vmm_read_aspace` / `vmm_write_aspace` zeros a real frame.
- Overlap → `STATUS_CONFLICTING_ADDRESSES`. Per-process commit cap `USER_COMMIT_MAX` 32 MiB.
- Kernel copy into an RX VAD is how ELF load works; a user write fault on that VAD still dies.
- User stack guard page is not in any VAD, so demand-zero will not fill it.

---

## T10 surface (0.10.0)

- `Ramdisk0` 1 MiB IRP device, `/dev/ram0` VNODE_BLOCK. Persist across close. OOB write `INVALID_PARAMETER`, overflow `DISK_FULL`.
- virtio-blk: identify legacy/modern, read legacy capacity from BAR0+20, I/O stays on Ramdisk0.
- IST `.bss` arrays gone. `tss_init` leaves IST=0; `tss_map_ist` after `vmm_init`.
- User `NtCreateThread` rejects `rip > USER_CANONICAL_TOP` on host too (not just hardware).

---

## T7 surface (0.7.0)

- User Read/Write bounce in `SYSCALL_COPY_MAX` chunks; hard cap still `COPY_MAX` 1 MiB.
- Pipe reader/writer counts follow handle insert and duplicate, not just create.
- Dead user processes drop out of `g_procs` on last thread (pid 0/1 stay).


---

## T6 surface (0.6.0)

- Trap frames always have ss/rsp/rflags/cs/rip (IST4 16 KiB IRQ stack).
- SMEP/SMAP set from CPUID before `sti`.
- Syscall bounce buffers for every user pointer (`SYSCALL_COPY_MAX` 64 KiB).
- `NtWaitForMultipleObjects` WAIT_ANY/WAIT_ALL, max 16.
- `NtQueryVirtualMemory` walks VADs.
- Pipe last-writer EOF via `object_type.close_fn`.
- `/dev/console` CHAR vnode. User ELF `/bin/hello` at 0x400000 with ntdll + libc printf/malloc.
- Kernel stack canary page (0xA5). Hardware unmapped kstack VA is residual — heap pages cannot punch a not-present hole.
