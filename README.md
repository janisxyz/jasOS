# jasOS

**Aegis** hybrid kernel. Bootable x86_64. Objects, handles, NTSTATUS,
a syscall gate, a panic that dumps registers.

This is an operating system. It is not a web app, not a Linux distro,
and not a toy "kernel" that `printf`s in ring 3.

```
firmware → Multiboot2 → long mode → Aegis → init → sh
```

## Repo

[https://github.com/janisxyz/jasOS](https://github.com/janisxyz/jasOS)

## What you get

- Higher-half kernel, identity map dropped after `vmm_init`
- Buddy PMM, slab heap, VAD-backed user virtual memory
- Object manager: Process, Thread, Section, File, Device, Event, Mutex, Timer, Directory
- Per-process handle tables with access masks
- Waitable dispatcher objects (real sleeps, not spinlocks named mutex)
- `syscall`/`sysret` ABI, NTSTATUS-shaped returns
- VFS + ramfs, IRP-shaped I/O
- Userland: `init`, `sh`, `ls`, `cat`, `echo`, `ps`, `crash`
- Host target that runs the same kernel C without QEMU (`make host`)

## Contracts (read these first)

| Doc | Lock |
|---|---|
| [ARCHITECTURE.md](ARCHITECTURE.md) | problem, threat model, address space, process model |
| [BOOT_CONTRACT.md](BOOT_CONTRACT.md) | Multiboot2, 32→64, `kmain_early` order, QEMU line |
| [SYSCALL_ABI.md](SYSCALL_ABI.md) | registers, status codes, copyin, table |
| [OBJECT_MODEL.md](OBJECT_MODEL.md) | header, handles, namespace, wait |
| [MEMORY.md](MEMORY.md) | buddy, slabs, VADs, lock rank |
| [SCHEDULER.md](SCHEDULER.md) | PIT quantum, priorities, switch frame |
| [BUILD.md](BUILD.md) | Windows/WSL toolchain, exact commands |

## Build and boot

Windows: WSL2 + `build-essential qemu-system-x86`. See [BUILD.md](BUILD.md).

```
make
make run
```

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

Without QEMU you can still execute the kernel's object manager, VFS,
scheduler and syscalls:

```
make host
./build/jasos-host --selftest
```

## License

MIT. See [LICENSE](LICENSE).
