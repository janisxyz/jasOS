# Boot contract

The loader and the kernel share this ABI. Break it and we triple-fault
before `kprintf` exists.

## Loader

Multiboot2. Magic on entry: `EAX = 0x36d76289`, `EBX` = physical address
of the multiboot2 info block (identity-mapped, < 4 GiB).

Header lives in `.multiboot_header`, 8-aligned, within the first 32 KiB
of the file. Tags:

1. Information request: memory map (`1`), framebuffer (`5`, optional).
2. Framebuffer tag: 80×25 text preferred; if GOP/linear FB is provided we
   still bring up COM1 first.
3. End tag.

Architecture field: `0` (i386). The 32-bit stub is required even though
the kernel is ELF64 — QEMU and GRUB both enter Multiboot2 kernels in
protected mode, paging off, 32-bit.

## CPU state on 32-bit entry (`_start`)

- 32-bit protected mode, paging **off**
- A20 enabled
- `cs` a 32-bit code selector; we immediately load our own GDT
- Interrupts disabled (`cli`) we keep them that way until IDT+PIC
- `EAX`/`EBX` as above; we stow them in `.data` before touching tables

## 32-bit stub obligations (`boot/entry.S`)

1. Load a temporary GDT (null / code32 / data32 / code64 / data64).
2. Identity-map the first 2 GiB with 2 MiB PAE entries (PML4[0], PDPT[0],
   512× PD). Also map the same 2 GiB at `0xFFFFFFFF80000000` (PML4[511]).
3. Enable PAE (`CR4.PAE`), set `EFER.LME`, enable paging (`CR0.PG`),
   far-jump into 64-bit CS.
4. Load a 64-bit GDT, set `rsp` to `boot_stack + BOOT_STACK_SIZE` (16 KiB,
   in `.bss`).
5. Jump to `kmain_early(u64 mb2_phys)` in C.

The 2 GiB identity map is a *boot crutch*. `vmm_init` replaces it with
the HHDM + kernel image mappings and unmaps the low identity map except
for the AP trampoline hole we do not yet have.

T8: `entry.S` sets `EFER.LME|EFER.NXE` together. Without NXE, T7's
`PTE_NX` is a reserved bit and the CR3 load #PFs. `vmm_init` keeps a
kernel-CR3-only 2 MiB identity (4 KiB leaves, same RO/RW split as
HHDM) so the boot stack survives the switch; user CR3 still has no
PML4[0]. VGA after `vmm_init` is HHDM-only (`serial_use_hhdm`).
`fpu_init` runs after `idt_init` and before `sti`.

T9: `tss_map_ist()` runs immediately after `vmm_init()`, before
`heap_init`. IST pointers in the TSS move from `.bss` to guarded
stacks. `sti` is still last.

T10: there is no `.bss` IST landing pad. `tss_init` leaves IST=0.
`tss_map_ist` is the only installer. `pci_init` identifies virtio-blk
and does not claim I/O. `io_init` brings up Serial0 and Ramdisk0.



## `kmain_early` order (do not reorder)

1. `serial_init()` — COM1 115200 8N1. If this fails we write VGA text
   `COM FAIL` and `hlt`.
2. Parse Multiboot2: mmap, command line, framebuffer. Copy mmap into a
   static array — the info block dies when we drop the identity map.
3. `gdt_init()` / `tss_init()` / `idt_init()` / `pic_remap(0x20, 0x28)`.
4. `pmm_init(mmap)` — every `available` entry except kernel image,
   boot stack, and the first 1 MiB (IVT/BIOS/MBR sludge).
5. `vmm_init()` — HHDM, kernel map, recursive slot, kernel heap window.
   Drop low identity. Reload `cr3`.
6. `heap_init()`.
7. `ob_init()` — `\ObjectTypes`, `\BaseNamedObjects`, `\Devices`, `\??`.
8. `sched_init()` — idle thread, BSP processor control block.
9. `vfs_init()` + `ramfs_mount("\\??\\RamDisk")` + `vfs_mount("/")`.
10. `syscall_init()` — `IA32_LSTAR`, `IA32_STAR`, `IA32_FMASK`, `IA32_EFER.SCE`.
11. `pit_init(100)` — 100 Hz. Unmask IRQ0. `sti`.
12. `psp_create_system_process()` then `psp_create_init()`.
13. `sched_start()` — does not return.

## Failure modes

| Failure | Action |
|---|---|
| No mmap | Panic `"no mmap"` |
| Less than 16 MiB free | Panic `"ram"` |
| Multiboot magic wrong | VGA `NO MB2` + `hlt` (serial may not be up) |
| Double fault | IST1 stack, dump, `hlt` |
| `kmain_early` returns | Panic `"kmain returned"` |

## QEMU inner loop (exact)

```
qemu-system-x86_64 ^
  -machine q35 ^
  -cpu qemu64,+syscall,+pae,+smep,+smap ^
  -m 256M ^
  -serial stdio ^
  -display none ^
  -no-reboot ^
  -no-shutdown ^
  -kernel build/kernel.elf
```

Windows PowerShell: same flags, `^` is `` ` ``.

GDB:

```
qemu-system-x86_64 ... -s -S -kernel build/kernel.elf
gdb build/kernel.elf -ex "target remote :1234" -ex "break kmain_early"
```

## Reversal log

none.

T6: every IDT gate uses an IST. IST1=#DF IST2=NMI IST3=#MC IST4=IRQ
(16 KiB). Kernel ticks no longer triple-fault. `cpu_enable_smap_smep`
runs after `syscall_init`/`kbd_init` and before `sti`. VGA text is
written at both identity `0xB8000` and `HHDM_BASE+0xB8000` because
we drop the identity map.
