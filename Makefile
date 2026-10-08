# hudos-server build — aarch64 (ARM64) UEFI application
# Toolchain: LLVM clang + lld-link (Homebrew), same as the hudos project.

# 解决非up系统环境使用clang编译提示/opt路径下缺少编译器的问题
# 去掉llvm绝对路径的参数
CC       := clang
LINK     := lld-link
TARGET   := aarch64-pc-win32-coff

CFLAGS := -target $(TARGET) -ffreestanding -fno-stack-protector -fshort-wchar \
          -fno-builtin -Wall -O2 -I.

BOOT := BOOTAA64.EFI

.PHONY: all clean

all: $(BOOT) img

$(BOOT): hudos_server.c efi.h mmu.c mmu_asm.S tls.c tls_bn.c tls_rsa.c x509.c tls_cli.c tls_handshake.c tls.h tls_bn.h tls_rsa.h tls_cli.h tls_handshake.h tls_roots.h userland/cjk16x16.h
	$(CC) $(CFLAGS) -c hudos_server.c -o hudos_server.o
	$(CC) $(CFLAGS) -c mmu.c -o mmu.o
	$(CC) $(CFLAGS) -c mmu_asm.S -o mmu_asm.o
	$(CC) $(CFLAGS) -c tls.c -o tls.o
	$(CC) $(CFLAGS) -c tls_bn.c -o tls_bn.o
	$(CC) $(CFLAGS) -c tls_rsa.c -o tls_rsa.o
	$(CC) $(CFLAGS) -c x509.c -o x509.o
	$(CC) $(CFLAGS) -c tls_cli.c -o tls_cli.o
	$(CC) $(CFLAGS) -c tls_handshake.c -o tls_handshake.o
	$(LINK) /entry:efi_main /subsystem:EFI_APPLICATION /machine:arm64 \
	        /dll /nodefaultlib /out:$(BOOT) hudos_server.o mmu.o mmu_asm.o \
	        tls.o tls_bn.o tls_rsa.o x509.o tls_cli.o tls_handshake.o

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
	rm -f hudos_server.o mmu.o mmu_asm.o $(BOOT) *.lib \
	rm -f *.o *.img *.qcow2 
