#!/bin/bash
# 最新版本的hudos, 已经提供好了编译后的镜像文件了
# 可以到https://github.com/xuchenruisz/hudos/releases下载
# dd用于建立磁盘, parted用于设置磁盘格式

set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

echo "[1/3] create img + setting img (dd + parted)..."
echo "creat img"
dd if=/dev/zero of=./hudos_server.img bs=256M count=1 status=progress
echo "setting img"
parted -s ./hudos_server.img mktable msdos
parted -s ./hudos_server.img mkpart primary fat32 1MiB 100%

echo "[2/3] executables into ESP root..."
MP="-i hudos_server.img@@1M" # FAT32 partition starts at LBA 2048 = 1 MiB
mformat -i ./hudos_server.img@@1M -F -v "HUDOS_ESP"
mmd -i ./hudos_server.img@@1M ::/EFI
mmd -i ./hudos_server.img@@1M ::/EFI/BOOT
mcopy $MP BOOTAA64.EFI ::/EFI/BOOT/BOOTAA64.EFI

echo "[3/3] converting raw -> qcow2..."
rm -f hudos_server.qcow2
qemu-img convert -f raw -O qcow2 hudos_server.img hudos_server.qcow2

echo "OK: $(ls -la hudos_server.qcow2 | awk '{print $5" bytes"}')  ->  hudos_server.qcow2"
