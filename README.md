## 关于本项目

**hodos** 是由双子星观测站开发的实验性操作系统内核项目。

原项目地址: https://github.com/xuchenruisz/hudos-

最新版本的hudos, 已经提供好了编译后的镜像文件了
可以到https://github.com/xuchenruisz/hudos/releases下载

---

## 本分支修改了一些编译方式, 没有修改源代码
1. 拉取原主的6.0新内核代码
2. 修改Makefile的编译器路径
3. 新增建立img文件和qemu启动脚本

关于编译参数

    make all                 一键编译全部
    make BOOTAA64.EFI        编译系统内核
    make img                 配置和设置光盘文件
    make clean               清理编译后的产物
    make run                 默认以img光盘镜像格式启动虚拟机
    make run-img             以img光盘镜像格式启动虚拟机
    make run-qcow2           以qemu的虚拟磁盘格式启动虚拟机


## 需要软件
. parted

. mtools

. qemu-system-arm

. clang

. llvm

. lld
