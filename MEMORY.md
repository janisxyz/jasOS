# Memory

## Physical

Buddy allocator. Orders 0..18 (4 KiB .. 1 GiB). Why buddy, not a
bitmap: splitting and coalescing is the cost model we want to feel
early; a bitmap is a weekend and a lie about fragmentation.

`pmm_init` walks the Multiboot2 mmap.

Reserved and never given out:

- `[0, 1 MiB)` — IVT, BDA, EBDA, VGA, BIOS
- Kernel image physical range `[kernel_phys, kernel_phys + size)`
- Boot page tables and boot stack (static in `.bss`, marked busy)
- Any mmap hole, ACPI NVS, bad RAM

Each free block is a `struct buddy_block` **stored in the page itself**
for order ≥ 0. Order-0 pages (single 4K) use a per-order intrusive list
of page-frame numbers, with metadata in a `frame_t` array allocated
from the first large chunk we steal at init (bitmap of *states*, not of
free-ness: `FREE / TAIL / KERNEL / USER / DMA / ZERO`).

`pmm_alloc(order, flags)`:

- `PMM_ZERO` — zero before return
- `PMM_DMA`  — below 16 MiB (ISA; we barely need it)
- `PMM_KERNEL` — account against kernel counter

Failure: return `PMM_INVALID` (`~0ULL`). Callers must handle it. The
heap's slab refill may then fail allocations up the stack with
`STATUS_NO_MEMORY`. We do not panic on OOM except in `kmain_early`
before any process exists.

## Virtual

4-level page tables, 4 KiB pages in v1. 2 MiB leaves are used only for
the HHDM and the kernel image. User maps are 4 KiB so COW/protect is
not a rewrite.

PTE flags: `P, RW, US, PWT, PCD, A, D, G, NX`. `G` on kernel image and
HHDM. `NX` on everything that is not an executable section.

`cr3` is per-process. Kernel maps (PML4[256..511]) are copied from the
template at process creation. After boot, kernel map changes (loadable
drivers, not v1) would need a shootdown; we do not have one, so the
kernel map is **frozen** after `vmm_init`. That is a contract.

Recursive PML4 slot 510: `va → pte` by the usual 9-bit dance. Used by
the page-fault handler and `vmm_map`. Not exposed to user.

## Kernel heap

Slab. Sizes: 16, 32, 64, 128, 256, 512, 1024, 2048, 4096.

Each slab is one or more pages. Objects have no header in the allocated
form; freelist next-pointer is stored in the free object. Poison on
free: `0xDB` (deadbeef minus the joke). Poison on alloc unless
`HEAP_ZERO`: `0xAA`.

`kalloc`/`kfree` are the only legal heap entry points. Drivers that
call `pmm_alloc` for long-lived DMA are fine; drivers that `pmm_alloc`
per IRP are going to be on the review tape.

Large allocations (> 4096) go to a buddy-backed `kalloc_pages` and are
4K aligned. `kfree` looks at a size-class table keyed by pointer (a
very small radix of slab addresses). Free of a pointer that is not
in any slab and not a large alloc: panic `"heap foreign"`.

Double-free: the poison and the freelist bitmap (1 bit per object in
the slab header) catch it. Panic `"heap df"`.

## User virtual

VADs. A process has a sorted list (later an AVL; v1 is a 64-entry
array because we are not mapping a browser).

`NtAllocateVirtualMemory` inserts a VAD, backs with demand-zero:
the PTE is invalid with a software `VAD_COMMIT` bit in the proto-PTE
(we store proto-PTEs as 8-byte entries in a side array for v1 — no,
we put a not-present PTE with bit 9 software `COMMITTED` and the rest
zero. PF handler zeros a real page and installs it).

v1 does **not** demand-page from files. `NtMapViewOfSection` of a file
section copies. That is a performance crime and a security simplification
we will reverse when the pager exists. Documented, not forgotten.

## Stacks

Kernel stack: 16 KiB + 4 KiB **canary** page filled with `0xA5`, checked
on `sched_exit_thread`. A not-present guard in the heap VA is a lie —
`kalloc` cannot punch a hole. Residual: dedicated `KERNEL_STACK_BASE`
region with an unmapped page, then IST1 is the backstop not the first
line. Overflow that eats the canary and the rest of the heap is still
a panic, just later.

User stack: 128 KiB default minus one page. `psp_exec_image` maps
`USER_STACK_SIZE - PAGE_SIZE` starting `PAGE_SIZE` above
`USER_STACK_TOP - USER_STACK_SIZE`, so the lowest page is a hole not
in any VAD. A PF there kills the thread (`vmm_handle_user_fault`
refuses addresses with no VAD). That *is* an unmapped guard.

## Lock ranking (memory)

```
0  panic   (never take a lock)
1  serial
2  pmm
3  heap
4  vmm per-aspace
5  vad
```

`pmm` may not call `kalloc`. `kalloc` may call `pmm`. `vmm` may call
both. PF handler may call `pmm` (to get a zero page) but **not** `kalloc`
(PF can run at DISPATCH). Zero-page refill is a DPC/work item in a
later pass; v1 zeros inline and eats the latency.

## Failure modes

| Mode | Result |
|---|---|
| OOM in syscall | `STATUS_NO_MEMORY`, no partial map left behind |
| OOM in PF for user | the thread takes `STATUS_NO_MEMORY` as if the syscall failed; if it was a true PF not in a syscall, we inject a fault that kills the thread |
| Kernel PF on unmapped | panic with cr2/cr3/error |
| NX execute | GP/PF, user thread dies, kernel panics |
| Non-canonical address | GP from the CPU; we treat like AV |

## Reversal log

none. Bitmap-first was the alternative; buddy won because coalescing
bugs are the ones we want to see in host tests.

T6: `vmm_unmap` frees the frame (it used to leak). `vmm_aspace_destroy`
walks user PML4[0..255], frees leftover leaves + tables, leaves kernel
half shared. Hardware `copyin`/`copyout`/`copyinstr` go through
`vmm_read_aspace`/`vmm_write_aspace` (HHDM), not user VA + STAC.
T7: kernel linker splits RX (text+rodata) and RW+NX (data+bss) on a
2 MiB boundary (`_rx_end`). `vmm_init` maps pages below `_rx_end`
without `PTE_W`, pages at/after it with `PTE_W|PTE_NX`. ELF PHDRs are
`PF_R|PF_X` and `PF_R|PF_W` — the RWX LOAD warning is gone. Residual:
HHDM still maps the kernel's physical pages writable. A kernel bug
that writes through HHDM can still clobber .text. Closing that is
excluding the kernel image from the HHDM or mapping it NX there.

T8: `EFER.NXE` is set in `entry.S` (and again in `vmm_init`) before any
`PTE_NX` is walked — T7's NX bits were ignored without it. HHDM 2 MiB
leaves are NX; the 2 MiB covering kernel RX is split to 4 KiB with
`PTE_W` stripped on `[kphys, _rx_end)`. VGA `0xB8000` and the boot
stack stay RW. Kernel CR3 keeps a 2 MiB identity of the same 4 KiB
PT (user CR3 does not copy PML4[0]). Kernel stacks live at
`KERNEL_STACK_BASE + (tid-1)*32KiB` with page 0 not-present; host
still uses the `0xA5` canary because it has no PTEs. Heap returns
16-byte-aligned objects (FXSAVE / GCC `movaps`). Residual: HHDM still
maps kernel `.data` and the rest of RAM writable — that is the
direct map, not a hole. IST stacks remain in `.bss`.

T9: IST1–4 relocated off `.bss` to `IST_STACK_BASE` (past the thread
kstack region) via `vmm_map_guarded_stack` after `vmm_init`, before
`sti`. Each has a not-present guard. Recursive PML4 slot is `PTE_NX`.
Residual: the `.bss` IST arrays are still linked; they are dead after
`tss_map_ist`.



