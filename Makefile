# jasOS / Aegis
CC      ?= gcc
LD      ?= ld
HOSTCC  ?= gcc
CROSS   ?=

KERNEL_CFLAGS = -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
	-mno-red-zone -mcmodel=kernel -m64 -mno-mmx -mno-sse -mno-sse2 \
	-Wall -Wextra -Wno-unused-variable -std=gnu11 -O2 -Ikernel/include -nostdlib

KERNEL_ASFLAGS = -ffreestanding -m64 -mno-red-zone -c

HOST_CFLAGS = -DJASOS_HOST -Wall -Wextra -std=gnu11 -O2 -g -Ikernel/include

KERNEL_C = \
	kernel/hal/serial.c \
	kernel/hal/gdt.c \
	kernel/hal/idt.c \
	kernel/hal/pic.c \
	kernel/hal/pit.c \
	kernel/lib/string.c \
	kernel/lib/status.c \
	kernel/ke/spin.c \
	kernel/ke/kprintf.c \
	kernel/ke/panic.c \
	kernel/ke/sched.c \
	kernel/ke/wait.c \
	kernel/ke/syscall.c \
	kernel/ke/kmain.c \
	kernel/mm/pmm.c \
	kernel/mm/vmm.c \
	kernel/mm/heap.c \
	kernel/ob/object.c \
	kernel/ob/handle.c \
	kernel/io/irp.c \
	kernel/fs/vfs.c \
	kernel/fs/elf.c \
	user/bin/init.c \
	user/bin/sh.c

KERNEL_S = \
	kernel/boot/entry.S \
	kernel/hal/isr.S \
	kernel/ke/switch.S

HOST_C = \
	kernel/hal/serial.c \
	kernel/lib/string.c \
	kernel/lib/status.c \
	kernel/ke/spin.c \
	kernel/ke/kprintf.c \
	kernel/ke/panic.c \
	kernel/ke/sched.c \
	kernel/ke/wait.c \
	kernel/ke/syscall.c \
	kernel/ke/kmain.c \
	kernel/mm/pmm.c \
	kernel/mm/vmm.c \
	kernel/mm/heap.c \
	kernel/ob/object.c \
	kernel/ob/handle.c \
	kernel/io/irp.c \
	kernel/fs/vfs.c \
	kernel/fs/elf.c \
	user/bin/init.c \
	user/bin/sh.c \
	host/main.c \
	host/selftest.c

.PHONY: all host kernel run clean selftest

all: host kernel

build:
	mkdir -p build

host: build
	$(HOSTCC) $(HOST_CFLAGS) -o build/jasos-host $(HOST_C)
	./build/jasos-host --selftest

kernel: build
	mkdir -p build/obj
	@for f in $(KERNEL_C); do \
	  echo "  CC  $$f"; \
	  $(CROSS)$(CC) $(KERNEL_CFLAGS) -c $$f -o build/obj/$$(echo $$f | tr / _).o || exit 1; \
	done
	@for f in $(KERNEL_S); do \
	  echo "  AS  $$f"; \
	  $(CROSS)$(CC) $(KERNEL_ASFLAGS) $$f -o build/obj/$$(echo $$f | tr / _).o || exit 1; \
	done
	$(CROSS)$(LD) -nostdlib -z max-page-size=0x1000 -T kernel/linker.ld \
	  -Map build/kernel.map -o build/kernel.elf build/obj/*.o
	@echo "kernel: build/kernel.elf"
	@ls -l build/kernel.elf

selftest: host

run: kernel
	qemu-system-x86_64 -machine q35 -cpu qemu64,+syscall,+pae -m 256M \
	  -serial stdio -display none -no-reboot -no-shutdown \
	  -kernel build/kernel.elf

gdb: kernel
	qemu-system-x86_64 -machine q35 -cpu qemu64,+syscall,+pae -m 256M \
	  -serial stdio -display none -no-reboot -no-shutdown \
	  -s -S -kernel build/kernel.elf

clean:
	rm -rf build
