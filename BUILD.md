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
  -cpu qemu64,+syscall,+pae \
  -m 256M \
  -serial stdio \
  -display none \
  -no-reboot \
  -no-shutdown \
  -kernel build/kernel.elf
```

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
`mm/`, `ob/`, `ke/`, `fs/` C as the kernel, with `host/hal_host.c`
providing serial, time, and a fake mmap of 128 MiB. Context switch is
`ucontext` (`swapcontext` onto each thread's kstack). If host tests fail,
the kernel is not "probably fine".

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

Serial is the console. There is no keyboard path in v1 besides COM1.
`kprintf` is lock-free on panic (reentrant flag); if you deadlock
the serial spinlock in a driver, panic still prints.

Symbols: `kernel.elf` is not stripped. `build/kernel.map` is written
by `ld -Map`.

## Layout of this tree

```
kernel/include/jasos/   public kernel headers
kernel/boot/            Multiboot2 + 32→64
kernel/hal/             serial, gdt, idt, pic, pit
kernel/mm/              pmm, vmm, heap
kernel/ob/              objects, handles, directories
kernel/ke/              kmain, sched, wait, syscall, panic
kernel/io/              IRP, device
kernel/fs/              VFS, ramfs
kernel/lib/             freestanding string/printf guts
user/                   libc subset + init/sh/ls/cat/echo/ps/crash
host/                   POSIX HAL + host entry
```
