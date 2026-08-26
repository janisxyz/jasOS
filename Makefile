# jasOS / Aegis — hybrid kernel. This is an operating system, not a website.
CC      ?= gcc
LD      ?= ld
HOSTCC  ?= gcc
CROSS   ?=

KERNEL_CFLAGS = -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
	-mno-red-zone -mcmodel=kernel -m64 -mno-mmx -mno-sse -mno-sse2 \
	-Wall -Wextra -Wno-unused-variable -std=gnu11 -O2 -Ikernel/include -Ibuild -nostdlib

KERNEL_ASFLAGS = -ffreestanding -m64 -mno-red-zone -c

HOST_CFLAGS = -DJASOS_HOST -D_GNU_SOURCE -Wall -Wextra -std=gnu11 -O2 -g -Ikernel/include -Ibuild

USER_CFLAGS = -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
	-mno-red-zone -m64 -mno-mmx -mno-sse -mno-sse2 \
	-Wall -Wextra -std=gnu11 -O2 -Ikernel/include -nostdlib

KERNEL_C = \
	kernel/hal/serial.c \
	kernel/hal/gdt.c \
	kernel/hal/idt.c \
	kernel/hal/pic.c \
	kernel/hal/pit.c \
	kernel/hal/pci.c \
	kernel/hal/kbd.c \
	kernel/lib/string.c \
	kernel/lib/status.c \
	kernel/ke/spin.c \
	kernel/ke/kprintf.c \
	kernel/ke/panic.c \
	kernel/ke/sched.c \
	kernel/ke/wait.c \
	kernel/ke/timer.c \
	kernel/ke/syscall.c \
	kernel/ke/exec.c \
	kernel/ke/kmain.c \
	kernel/mm/pmm.c \
	kernel/mm/vmm.c \
	kernel/mm/heap.c \
	kernel/ob/object.c \
	kernel/ob/handle.c \
	kernel/ob/pipe.c \
	kernel/ob/section.c \
	kernel/io/irp.c \
	kernel/fs/vfs.c \
	kernel/fs/elf.c \
	user/bin/init.c \
	user/bin/sh.c \
	user/bin/ls.c \
	user/bin/cat.c \
	user/bin/echo.c \
	user/bin/ps.c \
	user/bin/crash.c

KERNEL_S = \
	kernel/boot/entry.S \
	kernel/hal/isr.S \
	kernel/ke/switch.S \
	kernel/ke/syscall_entry.S \
	kernel/ke/enter_user.S

HOST_C = \
	kernel/hal/serial.c \
	kernel/lib/string.c \
	kernel/lib/status.c \
	kernel/ke/spin.c \
	kernel/ke/kprintf.c \
	kernel/ke/panic.c \
	kernel/ke/sched.c \
	kernel/ke/wait.c \
	kernel/ke/timer.c \
	kernel/ke/syscall.c \
	kernel/ke/exec.c \
	kernel/ke/kmain.c \
	kernel/mm/pmm.c \
	kernel/mm/vmm.c \
	kernel/mm/heap.c \
	kernel/ob/object.c \
	kernel/ob/handle.c \
	kernel/ob/pipe.c \
	kernel/ob/section.c \
	kernel/io/irp.c \
	kernel/fs/vfs.c \
	kernel/fs/elf.c \
	user/bin/init.c \
	user/bin/sh.c \
	user/bin/ls.c \
	user/bin/cat.c \
	user/bin/echo.c \
	user/bin/ps.c \
	user/bin/crash.c \
	host/main.c \
	host/selftest.c

.PHONY: all host kernel user run clean selftest iso

all: user host kernel

build:
	mkdir -p build build/obj build/initrd

host: build build/hello_blob.h
	$(HOSTCC) $(HOST_CFLAGS) -o build/jasos-host $(HOST_C)
	./build/jasos-host --selftest

kernel: build build/hello_blob.h
	mkdir -p build/obj
	@for f in $(KERNEL_C); do \
	  echo "  CC  $$f"; \
	  $(CROSS)$(CC) $(KERNEL_CFLAGS) -c $$f -o build/obj/$$(echo $$f | tr / _).o || exit 1; \
	done
	@for f in $(KERNEL_S); do \
	  echo "  AS  $$f"; \
	  $(CROSS)$(CC) $(KERNEL_ASFLAGS) $$f -o build/obj/$$(echo $$f | tr / _).o || exit 1; \
	done
	$(CROSS)$(LD) -nostdlib -z max-page-size=0x1000 -z separate-code -T kernel/linker.ld \
	  -Map build/kernel.map -o build/kernel.elf \
	  $$(for f in $(KERNEL_C) $(KERNEL_S); do echo build/obj/$$(echo $$f | tr / _).o; done)
	@echo "kernel: build/kernel.elf"
	@ls -l build/kernel.elf
	@readelf -h build/kernel.elf | head -20 || true

user: build
	mkdir -p build/userobj build/initrd
	$(CROSS)$(CC) $(USER_CFLAGS) -c user/crt0.S -o build/userobj/crt0.o
	$(CROSS)$(CC) $(USER_CFLAGS) -c user/ntdll/ntdll.S -o build/userobj/ntdll.o
	$(CROSS)$(CC) $(USER_CFLAGS) -c user/bin/hello.c -o build/userobj/hello.o
	$(CROSS)$(CC) $(USER_CFLAGS) -c user/libc/printf.c -o build/userobj/printf.o
	$(CROSS)$(CC) $(USER_CFLAGS) -c user/libc/malloc.c -o build/userobj/malloc.o
	$(CROSS)$(CC) $(USER_CFLAGS) -c kernel/lib/string.c -o build/userobj/string.o
	$(CROSS)$(LD) -nostdlib -T user/linker.ld -o build/initrd/hello \
	  build/userobj/crt0.o build/userobj/ntdll.o build/userobj/hello.o \
	  build/userobj/printf.o build/userobj/malloc.o build/userobj/string.o
	python3 scripts/embed_elf.py build/initrd/hello build/hello_blob.h hello_elf_blob
	@echo "user: build/initrd/hello"
	@ls -l build/initrd/hello
	@file build/initrd/hello || true

build/hello_blob.h:
	mkdir -p build
	@if [ ! -f build/hello_blob.h ]; then \
	  printf '%s\n' '/* stub until make user */' \
	    'static const unsigned char hello_elf_blob[] = {0};' \
	    'static const unsigned int hello_elf_blob_len = 0;' > build/hello_blob.h; \
	fi

run: kernel
	qemu-system-x86_64 -machine q35 -cpu qemu64,+syscall,+pae,+smep,+smap -m 256M \
	  -serial stdio -display none -no-reboot -no-shutdown \
	  -kernel build/kernel.elf

gdb: kernel
	qemu-system-x86_64 -machine q35 -cpu qemu64,+syscall,+pae,+smep,+smap -m 256M \
	  -serial stdio -display none -no-reboot -no-shutdown \
	  -s -S -kernel build/kernel.elf

iso: kernel
	mkdir -p build/isodir/boot/grub
	cp build/kernel.elf build/isodir/boot/jasos.elf
	printf 'set timeout=0\nset default=0\nmenuentry "jasOS Aegis" {\n  multiboot2 /boot/jasos.elf\n  boot\n}\n' \
	  > build/isodir/boot/grub/grub.cfg
	grub-mkrescue -o build/jasos.iso build/isodir

clean:
	rm -rf build

selftest: host
