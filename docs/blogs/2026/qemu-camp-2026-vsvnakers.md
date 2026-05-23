# QEMU 训练营 2026 专业阶段总结

!!! note "主要贡献者"

    - 作者：[vsvnakers](https://github.com/vsvnakers)

---

## 背景介绍


微电子专业在读，在校大四本科生，往计算机方向转对底层系统技术比较感兴趣。看到 QEMU 训练营有机会从源码层面深入理解一个工业级虚拟机的内部实现，觉得是一次很好的学习机会，于是报名参加了专业阶段。

## 专业阶段

专业阶段共四个实验方向，应该完成了。以下简要记录。

### CPU 实验

在 TCG 中实现 10 条自定义 RISC-V 指令（Xg233ai 扩展），包括 vadd、dma、sort、crush、expand、vdot、gemm、vrelu、vscale、vmax。其中 5 条用纯 TCG IR 实现，5 条通过 helper 函数实现。主要修改了 `insn32.decode`、`trans_xg233.c.inc`（新建）、`helper.h`、`op_helper.c`、`translate.c` 共 5 个文件。

过程中对 TCG 的工作流有了实际体会：指令解码 → 中间码生成 → 宿主机代码生成。踩了不少坑，比如 `vrelu` 需要符号扩展（`MO_TESL`）、helper 函数中内存访问要用 `cpu_ldl_mmu`/`cpu_stl_mmu`、`#ifndef CONFIG_USER_ONLY` 保护等。

### SoC 实验

为 G233 虚拟 SoC 实现了 4 个外设模型：GPIO、PWM、WDT、SPI+Flash。每个外设都是一个 `SysBusDevice`，通过 MMIO 回调处理寄存器读写。GPIO 有边沿检测和 W1C，PWM 有 4 通道定时器，WDT 有 feed/lock 机制，SPI+Flash 有完整的命令状态机。

改动规模较大，新增 10 个文件、修改 8 个文件，涉及 Kconfig 和 meson.build 的构建系统配置。印象最深的是 Flash 的 WEL 位在 CS 拉高时应该清除，这个细节导致 3 个测试失败。

### GPGPU 实验

完成 GPGPU PCI 设备的控制逻辑和 SIMT 执行引擎。设备通过 PCI BAR 暴露控制寄存器、VRAM 和 Doorbell，支持 32-lane warp 的 SIMT 锁步执行。主要工作是实现 8 种低精度浮点格式（BF16、E4M3、E5M2、E2M1）的双向转换函数，以及 warp 执行引擎和 kernel dispatch 逻辑。

只修改了 `gpgpu.c` 和 `gpgpu_core.c` 两个文件，但信息密度很高。最大的坑是 funct7 是 7 位而不是 8 位，导致所有自定义浮点指令的编码全错，排查了很久。

### Rust 实验

用 Rust 实现 I2C 总线模型和 AT24C02 EEPROM，用 C 实现 I2C GPIO 控制器和 RSPI 控制器，通过 FFI 桥接。Rust 侧编译为静态库，导出 7 个 C 接口函数；C 侧通过这些接口操作 I2C 总线。

这是第一次在 QEMU 中使用 Rust，对 C-Rust FFI 的生命周期管理（`Box::into_raw`/`Box::from_raw`）有了实际理解。AT24C02 的页写入回绕和 AT25 Flash 的状态机是主要难点。

## 总结

四个实验覆盖了 QEMU 内部不同层次的知识：CPU 实验对应 TCG 翻译层，SoC 实验对应 QOM 设备模型和 MMIO，GPGPU 对应 PCI 设备和 SIMT 执行，Rust 对应跨语言 FFI。全部做下来之后，对 QEMU 从指令翻译到设备模拟的完整链路有了比较系统的认识。

给后续学员的建议：

- 每个实验建议按难度递进的顺序实现，先跑通一个最简单的测试，再逐步扩展
- 善用 GDB 调试，QEMU 的代码量很大，单靠读代码很难定位问题
