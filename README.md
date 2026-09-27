## 关于本项目

**hodos** 是由双子星观测站开发的实验性操作系统内核项目。

原项目地址: https://github.com/xuchenruisz/hudos-

## 本分支修改了一些编译方式, 没有修改源代码
1. 修改了userland中的makefile文件
2. 修改了主文件下的build_disk.sh文件
3. 主文件下新增Makefile文件, 方便大家编译

关于编译参数

    make all         一键编译全部
    make efi         编译系统引导, 或者内核?
    make elf         编译用户空间的程序
    make img         配置和设置光盘文件
    make clean       清理编译后的产物
    make run         默认以img光盘镜像格式启动虚拟机
    make run-img     以img光盘镜像格式启动虚拟机
    make run-qcow2   以qemu的虚拟磁盘格式启动虚拟机


## 需要软件
. parted

. qemu-system-arm

. clang

. llvm