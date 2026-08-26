# Build

## Toolchain

| Tool | Why |
|---|---|
| `gcc` ≥ 12 or `clang` ≥ 16 | C11 freestanding, `mcmodel=kernel` |
| GNU `as` / `ld` (binutils) | GAS boot/entry, linker script |
| `qemu-system-x86_64` | run the kernel |
| `gdb` | `-s -S` stub |

No NASM. No Meson. No CMake. `make`.

### Windows

1. Install WSL2, Ubuntu 22.04+.
2. Inside WSL:

```
sudo apt update
sudo apt install -y build-essential qemu-system-x86 gdb
```

3. Hyper-V users: QEMU works inside WSL2. You can also copy
   `build/kernel.elf` and run it under a Hyper-V Gen2 VM with a GRUB
   ISO; that is not the inner loop.

Git-bash + MinGW cannot build this: we need ELF, not PE, and
`-mcmodel=kernel`. WSL is the supported Windows path.

### Linux / macOS

Linux: `build-essential qemu-system-x86`.

macOS: `x86_64-elf-gcc` from brew (`i386-elf-gcc` is wrong) plus
`qemu`. The Makefile honours `CROSS=x86_64-elf-`.

## Targets

```
make             # host tests + kernel.elf
make host        # POSIX HAL, runs object/VFS/sched tests
make kernel      # freestanding build/kernel.elf
make user        # user ELFs into build/initrd (needs kernel headers)
make run         # qemu -kernel build/kernel.elf -serial stdio
make gdb         # qemu -s -S, wait for gdb
make clean
```

## Exact QEMU command

```
qemu-system-x86_64 \
  -machine q35 \
  -cpu qemu64,+syscall,+pae,+smep,+smap \
  -m 256M \
  -serial stdio \
  -display none \
  -no-reboot \
  -no-shutdown \
  -kernel build/kernel.elf
```

T10: `make run` also creates an 8 MiB `build/disk.img` and attaches
`virtio-blk-pci`. The kernel identifies the device; block I/O in this
pass is Ramdisk0 (`/dev/ram0`), not virtqueues.

With a GRUB ISO (optional, needs `grub-mkrescue` + `xorriso`):

```
make iso
qemu-system-x86_64 -machine q35 -m 256M -serial stdio -cdrom build/jasos.iso
```

## Flags (kernel)

```
-ffreestanding -fno-stack-protector -fno-pic -fno-pie
-mno-red-zone -mcmodel=kernel -m64 -mno-mmx -mno-sse -mno-sse2
-Wall -Wextra -std=gnu11 -O2 -nostdlib -lgcc
```

`-mno-red-zone` is not optional. ABI red zone + interrupts = silent
corruption.

`-mno-sse`: kernel context switch does not FXSAVE in v1.

## Host tests

`make host` produces `build/jasos-host` and runs it. This is the same
`mm/`, `ob/`, `ke/`, `fs/` C as the kernel, with `host/main.c` providing
serial, time, and a fake mmap of 128 MiB. Context switch is `ucontext`
(`swapcontext` onto each thread's kstack + 4 KiB canary). If host tests
fail, the kernel is not "probably fine".

0.6 selftest also covers handle generation, `/dev/console`,
`NtQueryVirtualMemory`, `NtWaitForMultipleObjects` WAIT_ANY, and pipe
last-writer EOF.

0.12 selftest also covers wait-on-thread/process, abandoned mutex,
priority inheritance, `NtTerminateThread` by handle, handle inherit,
and per-page host shadow (1 MiB VAD + 1 byte write).

0.14 selftest also covers VAD split (prefix/middle/suffix protect and
free), coalesce after restore, size-0 whole-VAD free, and WAIT_ALL
on a mutex the caller already owns.

0.15 selftest also covers `/bin/echo` as ELF (not a kernel builtin),
32-page populated free + hole realloc, and size-0 release after that.

0.16 selftest also covers `NtCreateProcessEx` argv cap/null/stack
round-trip (`/bin/echo hello from argv`), Token as `OBJ_TOKEN`,
`NtOpenProcessToken` rights (`PROCESS_QUERY_INFORMATION`,
`TOKEN_QUERY` vs `TOKEN_DUPLICATE`), `NtDuplicateToken` independence,
drop-only `NtSetInformationToken`, and System pid 0 token.




```
./build/jasos-host
./build/jasos-host --selftest
./build/jasos-host --shell     # interactive, stdin is the serial
```

## Debugging

```
make gdb
# other terminal:
gdb build/kernel.elf
(gdb) target remote :1234
(gdb) break kmain_early
(gdb) continue
```

Serial is COM1. Keyboard IRQ1 is drained into a small ring (`kbd.c`);
the shell still prefers serial because that is what QEMU `-serial stdio`
is. `kprintf` is lock-free on panic.

Symbols: `kernel.elf` is not stripped. `build/kernel.map` is written
by `ld -Map`.

## 0.6 QEMU CPU flags

`+smep,+smap` so QEMU's qemu64 actually presents the bits our
`cpu_enable_smap_smep` looks for. Without them the kernel boots with
SMEP=0 SMAP=0 and says so. That is honest, not a skip.

## Layout of this tree

```
kernel/include/jasos/   public kernel headers
kernel/boot/            Multiboot2 + 32→64
kernel/hal/             serial, gdt, idt, pic, pit, kbd
kernel/mm/              pmm, vmm, heap
kernel/ob/              objects, handles, directories, pipe, section
kernel/ke/              kmain, sched, wait, syscall, panic, exec
kernel/io/              IRP, device
kernel/fs/              VFS, ramfs, ELF
kernel/lib/             freestanding string/printf guts
user/                   libc subset + init/sh/ls/cat/echo/ps/crash + hello
host/                   POSIX HAL + host entry
scripts/                embed_elf.py
```

0.17 selftest also covers child token inherit after parent drop (cannot
spawn up), System child still admin, `/bin/ls` and `/bin/cat` as ELF
(not kernel builtins), and argv on those images.


0.17 also covers `/bin/ps` as ELF (T20 start).

