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
| `sysret` to kernel RIP | documented; gate not yet the user path (kernel-linked v1) |
| ELF PT_INTERP / ET_DYN | refused |
| pid 1 exit | panic `"init died"` — no "return to firmware" lie |

## Residual (honest)

| Residual | Why it still exists | Mitigation |
|---|---|---|
| Kernel-linked userland | ELF user programs are loaded by `elf_load` but init/sh are linked into the kernel for v1 so we have a shell the day the box boots | Treat sh as ring 0. Do not ship this as a security boundary. Next: user CR3 + syscall gate. |
| No SMEP/SMAP | HAL does not set CR4.SMEP/SMAP | Gate not live; once it is, set both before `sti` |
| No KASLR | kernel at `0xFFFFFFFF80000000` | Fine until we have a UEFI stub with entropy |
| FXSAVE missing | SSE from user clobbers kernel XMM | Kernel built `-mno-sse`; do not enable the gate until FXSAVE |
| VAD array 64 | a mapping bomb fails closed with `INSUFFICIENT_RESOURCES` | fail closed is the mitigation |
| Recursive PML4 | if a user map ever got slot 510, they own page tables | `vmm_map` of user aspace copies kernel half from template; user `NtMap` rejects high VA |
| Host `copyin` is memcpy | host HAL is not a security boundary | hardware path probes PTEs |
| Admin == kernel | Token is a field, not an object | do not claim otherwise |

## Panic path contract

`panic` sets `g_panic_in_progress` before it prints. `kprintf` then
skips the serial spinlock. `spin_lock` will not spin forever. The
dump includes thread name, pid, ticks, irql, held rank. Then `hlt`.
We do not "recover". A recovered kernel after DF is a lying kernel.
