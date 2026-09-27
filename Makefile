#  hudos-server — a minimal Unix-like OS / shell that runs as a UEFI
#  application (aarch64 / ARM64 EFI). It boots from the ESP, presents a
#  text CLI (ConOut, captured by QEMU -serial), implements ls/cd/mkdir/
#  open/del/help/copy/pwd/cls, and can LOAD & RUN aarch64 ELF executables
#  via the `open` command. Userland programs are freestanding PIE ELF files
#  compiled for this platform; they talk to the OS through a tiny syscall
#  shim passed in register x3 (no SVC / exceptions needed).
# 
#  Build with clang + lld-link (aarch64-pc-win32-coff), see Makefile.
#  原主貌似没有提供Makefile文件, 所以以下是我自己写的

CC        := clang
LD        := lld-link

.PHONY: all clean
all: clean efi img

efi: efi.h hudos_server.c
	$(CC) -target aarch64-unknown-windows-gnu \
	-ffreestanding \
	-fshort-wchar \
	-mno-red-zone \
	-c hudos_server.c \
	-o hudos_server.o
	$(LD) -subsystem:efi_application \
	-entry:efi_main \
	-machine:arm64 \
	hudos_server.o -out:BOOTAA64.EFI
	@echo "output: hudos_server.o & BOOTAA64.EFI"

elf:
	$(MAKE) -C userland

img: build_disk.sh
	bash build_disk.sh

run:
	$(MAKE) run-img

run-img:
	qemu-system-aarch64 \
	-machine virt \
	-cpu cortex-a72 \
	-m 2G \
	-smp 4 \
	-bios QEMU_EFI.fd \
	-drive file=hudos_server.img,format=raw \
	-device virtio-gpu-pci \
	-display sdl \
	-device qemu-xhci,id=xhci \
	-device usb-tablet,bus=xhci.0 \
	-device usb-kbd,bus=xhci.0

run-qcow2:
	qemu-system-aarch64 \
	-machine virt \
	-cpu cortex-a72 \
	-m 2G \
	-smp 4 \
	-bios QEMU_EFI.fd \
	-drive file=hudos_server.qcow2,format=qcow2 \
	-device virtio-gpu-pci \
	-display sdl \
	-device qemu-xhci,id=xhci \
	-device usb-tablet,bus=xhci.0 \
	-device usb-kbd,bus=xhci.0

clean:
	rm -rf BOOT* \
	*.o \
	*.img \
	*.qcow2 \
	userland/*.o \
	userland/*.elf