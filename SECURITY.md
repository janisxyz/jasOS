# Security pass (evil)

What a usermode process can do to the kernel **today**, and what we
closed vs residual.

## Closed this pass

| Hole | Fix |
|---|---|
| User pointer deref in syscall | `copyin`/`copyout`/`copyinstr` + canonical + size cap 1 MiB |
| Handle reuse after close | slot cleared, `INVALID_HANDLE` on double close (selftest) |
| Generic access not expanded | `ob_map_generic` at insert |
| Mutex release by anyone | owner **and** recursion checked; NULL current is not owner |
| Wait on non-waitable | `o->wait == NULL` → `INVALID_PARAMETER` |
| Path `..` escape | `path_norm` eats `..` against cwd, never below `/` |
| Kernel VA in `NtAllocateVirtualMemory` | reject `> USER_CANONICAL_TOP` |
| Panic taking the serial lock | `g_panic_in_progress` + kprintf depth; panic prints anyway |
| Lock rank inversion (heap→pmm, vfs→heap) | drop higher-rank locks before allocating |
| `sysret` to kernel RIP | canonical check in `syscall_entry.S` (`shr 47` / `ud2`) |
| ELF PT_INTERP / ET_DYN | refused |
| pid 1 exit | panic `"init died"` — no "return to firmware" lie |
| Stale handle after close | `HANDLE_VALUE(i, gen)`; lookup rejects gen mismatch; close bumps gen (selftest) |
| Same-CPL IRQ frame | IST4 so PIT in kernel always pushes ss/rsp |
| User RIP in kernel | `NtCreateThread` on `user_mode` process uses `enter_user`, never `call` |
| copyin through user VA | hardware path: `vmm_probe_user` + `vmm_read_aspace` via HHDM; SMAP stays on |
| Syscall user pointers | marshalling bounce for Create/Read/Write/path/Event/Mutex/Section/VM/Pipe/Query/* |
| SMEP/SMAP off | `cpu_enable_smap_smep` CPUID-gated, before `sti` |
| Process exit leak | last thread `vmm_aspace_destroy` (walk user half, free frames+tables) + `ht_destroy` |
| Pipe dup EOF | `open_fn` increments readers/writers on insert and duplicate |
| Syscall bounce DoS | user Read/Write chunked at `SYSCALL_COPY_MAX`; hard cap `COPY_MAX` |
| HHDM RWX over `.text` | HHDM is NX; kernel RX phys pages are RO in HHDM (`fill_lo_pt`) |
| `PTE_NX` ignored | `EFER.NXE` in `entry.S` before long-mode CR3 |
| W^X ELF | `elf_load` policy pass refuses `PF_W|PF_X` PT_LOAD and `PF_X` PT_GNU_STACK |
| Kernel stack overflow into heap | hardware kstack at `KERNEL_STACK_BASE`, guard page not-present |
| SSE clobber | lazy FXSAVE on `#NM`; kernel `#NM` panics; `CR0.TS` on switch |
| `kalloc` unaligned | slab/large payload 16-byte aligned (selftest) |
| IST stacks in `.bss` | `tss_map_ist` maps guarded stacks at `IST_STACK_BASE` before `sti` |
| Recursive PML4 executable | slot 510 is `PTE_P|PTE_W|PTE_NX` |
| IST `.bss` landing pad | arrays deleted; IST=0 until `tss_map_ist` |
| User `NtCreateThread` kernel RIP on host | rejected with `ACCESS_VIOLATION` before the HOST ifdef |
| Ramdisk dmesg lie | real 1 MiB backing, IRP dispatch, `/dev/ram0` |
| `NtAllocate` pre-backed every page | demand-zero + `PTE_SW_COMMIT`; PF/copyin populate |
| copyin required `PTE_P` | hardware probe is VAD+prot; populate on copy |
| Mapping bomb hits PMM | `USER_COMMIT_MAX` 32 MiB + overlap reject |
| Mutex owner death hangs waiters | owned-mutex list; death abandons with `STATUS_ABANDONED` |
| `wait_boost` was dead | waiter donates via `sched_boost`; unwind when no mutexes remain |
| `NtTerminateThread` self-only | handle lookup + `kill_pending`; idle refused |
| Inherit bit always 0 | `ht_set_inherit` / `DUPLICATE_INHERIT` / `ht_inherit_table` on create |
| Host whole-VAD shadow | per-page 4 KiB; 1 MiB VAD + 1 byte write stays under 32 KiB heap |
| `ht_destroy` kfree under HANDLE | snapshot slot, drop lock, then close/deref |
| `g_procs` UAF | table ref on insert; reap derefs; close cannot free a tabulated process |
| User `NtProtect` to W^X | refused `INVALID_PAGE_PROTECTION` |
| Exact-match protect/free | split a containing VAD; size 0 free is whole region |
| `vmm_free_user` ignored size | a 1-page free of an 8-page VAD no longer nukes the rest |
| NOACCESS then RW leaked the frame | `apply_prot_range` restores `pa` when `PTE_P` is clear |
| WAIT_ALL poll on owned mutex | `disp_satisfied` counts owner==self even when `signal_state==0` |
| Unmap `pmm_free` under VMM | collect PAs, drop VMM, then `pmm_free` (T15) |
| `vmm_map` `pt_alloc` under VMM | `vmm_ensure_leaf` allocates the table first |
| `/bin/echo` kernel-linked | unlinked; ET_EXEC + ntdll; sh builtin echo remains ring 0 |
| NtCreateProcess argv missing | T16: a4/a5 copyin + stack image; cap 16×128 |
| Token was a Process field | T17: `OBJ_TOKEN`; open needs `PROCESS_QUERY_INFORMATION`; query needs `TOKEN_QUERY` |
| TOKEN_DUPLICATE was a dead bit | T17: `NtDuplicateToken` copies pid+integrity into a new object |
| Integrity could be raised | T18: `NtSetInformationToken` drop-only |

## Residual (honest)

| Residual | Why it still exists | Mitigation |
|---|---|---|
| Kernel-linked userland | init/sh/ls/cat/ps/crash are still linked into kernel.elf so the box has a shell the day it boots | Treat sh as ring 0. `/bin/hello` and `/bin/echo` are the ring-3 path. |
| No KASLR | kernel at `0xFFFFFFFF80000000` | Fine until a UEFI stub with entropy |
| No XSAVE | FXSAVE is 512 bytes; AVX is not covered | Do not set `CR4.OSXSAVE` |
| VAD array 64 | a mapping bomb fails closed with `INSUFFICIENT_RESOURCES` | fail closed is the mitigation; commit cap is the other |
| Recursive PML4 | if a user map ever got slot 510, they own page tables | user `NtMap` rejects high VA; user half of CR3 is private; slot is NX |
| Host `copyin` is memcpy | host HAL is not a security boundary | hardware path probes VADs and copies via HHDM |
| Admin == kernel | Token is an object; integrity still starts at 1 | do not claim logon exists; drop is 1→0 only |
| HHDM maps RAM RW | heap and copyin live there | kernel `.text` is RO in HHDM; user never has HHDM |
| virtio-blk virtqueues | identify-only; I/O is Ramdisk0 | do not print "virtio I/O up" |
| PIC not LAPIC | 8259 is v1; MADT/x2APIC is the next HAL | dmesg says 8259, never LAPIC |
| Protect spans mixed VADs | one containing VAD only; coalesce is same-prot adjacent | fail closed `CONFLICTING_ADDRESSES` |
| OOM unmap batch leak | >16-page free with kalloc fail batches 16; populate can refill a cleared page | fail closed is rare; commit cap keeps n_pages bounded |
| NtCreateProcess has no envp | stack writes a single env NULL | no 7th syscall argument; T18 residual |
| No privileges bitmap | Token has pid+integrity only | `TOKEN_ADJUST` cannot grant Se*; there is no Se* |


## Panic path contract

`panic` sets `g_panic_in_progress` before it prints. `kprintf` then
skips the serial spinlock. `spin_lock` will not spin forever. The
dump includes thread name, pid, ticks, irql, held rank. Then `hlt`.
We do not "recover". A recovered kernel after DF is a lying kernel.

| Spawn re-elevated after drop | T19: child integrity = parent integrity (clamped 0..1) |
| `/bin/ls` `/bin/cat` kernel-linked | unlinked; ET_EXEC + ntdll |

T19 residual update: kernel-linked remainder is init/sh/ps/crash. Drop is
sticky on spawn. integrity still *starts* at 1 for System and its
undropped children. No Se* bitmap. envp is still one NULL.

