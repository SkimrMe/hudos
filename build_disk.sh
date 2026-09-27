#!/bin/bash
# Build a bootable hard-disk image for hudos-server:
#   1. compile the EFI app (BOOTAA64.EFI) and the userland ELF programs
#   2. use the proven hudos/fatgen to lay down an MBR + FAT32 ESP containing
#      \EFI\BOOT\BOOTAA64.EFI  (guaranteed to boot under ArmVirtQemu)
#   3. drop the userland *.elf files into the ESP root with mtools (mcopy)
#   4. optionally add an AUTOTEST marker file (run with: ./build_disk.sh autotest)
#   5. convert the raw image to qcow2

# 因为缺少了fatgen工具, 顾而改成使用使用dd和parted
# dd用于建立磁盘, parted用于设置磁盘格式

set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

echo "[1/5] building EFI app + userland..."
make -C userland

echo "[2/5] lcreat img + setting img (dd + parted)..."
echo "creat img"
dd if=/dev/zero of=./hudos_server.img bs=256M count=1 status=progress
echo "setting img"
parted -s ./hudos_server.img mktable msdos
parted -s ./hudos_server.img mkpart primary fat32 1MiB 100%

echo "[3/5] injecting userland executables into ESP root..."
MP="-i hudos_server.img@@1M"   # FAT32 partition starts at LBA 2048 = 1 MiB
mformat -i ./hudos_server.img@@1M -F -v "HUDOS_ESP"
mmd -i ./hudos_server.img@@1M ::/EFI
mmd -i ./hudos_server.img@@1M ::/EFI/BOOT
mcopy $MP BOOTAA64.EFI ::/EFI/BOOT/BOOTAA64.EFI
mcopy $MP userland/hello.elf ::/hello.elf
mcopy $MP userland/echo.elf  ::/echo.elf
mcopy $MP userland/calc.elf  ::/calc.elf

if [ "$1" = "autotest" ]; then
    echo "[4/5] adding AUTOTEST marker (auto-run self-test on boot)..."
    touch AUTOTEST
    mcopy $MP AUTOTEST ::/AUTOTEST
    rm -f AUTOTEST
else
    echo "[4/5] (no AUTOTEST marker; interactive shell on boot)"
fi

echo "[5/5] converting raw -> qcow2..."
rm -f hudos_server.qcow2
qemu-img convert -f raw -O qcow2 hudos_server.img hudos_server.qcow2

echo "OK: $(ls -la hudos_server.qcow2 | awk '{print $5" bytes"}')  ->  hudos_server.qcow2"
