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

## Residual (honest)

| Residual | Why it still exists | Mitigation |
|---|---|---|
| Kernel-linked userland | init/sh/ls/cat/echo/ps/crash are still linked into kernel.elf so the box has a shell the day it boots | Treat sh as ring 0. `/bin/hello` is the ring-3 path. |
| No KASLR | kernel at `0xFFFFFFFF80000000` | Fine until a UEFI stub with entropy |
| FXSAVE missing | SSE from user clobbers kernel XMM | Kernel built `-mno-sse`; do not enable XMM in user until FXSAVE |
| VAD array 64 | a mapping bomb fails closed with `INSUFFICIENT_RESOURCES` | fail closed is the mitigation |
| Recursive PML4 | if a user map ever got slot 510, they own page tables | user `NtMap` rejects high VA; user half of CR3 is private |
| Host `copyin` is memcpy | host HAL is not a security boundary | hardware path probes PTEs and copies via HHDM |
| Admin == kernel | Token is a field, not an object | do not claim otherwise |
| Kernel stack guard | canary page of `0xA5`, not a not-present PTE | IST1 still catches DF; canary panics on exit |


## Panic path contract

`panic` sets `g_panic_in_progress` before it prints. `kprintf` then
skips the serial spinlock. `spin_lock` will not spin forever. The
dump includes thread name, pid, ticks, irql, held rank. Then `hlt`.
We do not "recover". A recovered kernel after DF is a lying kernel.
