## farmOS内容一览

### 启动程序
#### /kern/boot/entry.S
汇编系统入口，初始化全局内核栈kstack,kstack静态在数据段定义

	和用户也有自己的内核栈不冲突，全局内核栈在异常处理和早起初始化中使用。
	在用户程序出现之前，sp总得有个地方去，对吧？

#### /kern/boot/start.c
接收引导程序传来的dtb_entry地址
初始化全局dtb_entry

#### /kern/boot/main.c
初始化设备树 /kern/dev/dtb.c

	设备树已经在Opensbi中经过一次处理了，然后再传递给内核进行进一步处理。
	opensbi类似于pke中的spike仿真器。
	QEMU 提供 SBI 设备模拟，让 OpenSBI 能够在 QEMU 里运行，并与 S-mode 内核交互。
	SBI 调用本质上是 ecall 指令，S-mode 通过 ecall 陷入 M-mode，由 OpenSBI 处理请求并返回结果。
	解析设备树，最主要的目的是解析出meminfo。这个设备树文件时qemu提供的（根据qemu的启动参数）。
	这个全局meminfo的使用场景为：
	- main启动时打印内存大小
	- pmminit
	- sys_info


