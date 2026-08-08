# MTK 屏幕超频 LKM：PMB110 示例

英文说明：[README.md](README.md)

本项目是面向 MTK 平台的屏幕超频 LKM 参考实现，以 PMB110 作为示例设备。
示例模块在系统运行时向 Android 显示栈增加 170Hz 和 185Hz 档位，覆盖官方
提供的两种分辨率。启用后，所有档位使用统一的 1496Mbps MIPI DSI 链路；官方档位通过补偿
垂直消隐区保持原来的目标刷新率，新增档位复用已经验证过的 165Hz 面板
DDIC 命令路径并使用新的主机时序。

## PMB110 示例基线

仓库中的示例实现针对 PMB110 的软件、面板和内核 ABI：

- 产品：PMB110
- SoC：MediaTek MT6993
- 面板：`panel_aa618_p_3_a0034_dsi_vdo`
- 官方软件：`PMB110_16.0.9.400(CN01)`
- 内核版本：`6.12.58-android16-6-g7704a1ae279b-ab15213644-4k`
- 内核分支：`oneplus/mt6993_b_16.0_ace_6_ultra`
- 内核源码提交：`2e3b6d890bd2c7bd5779bef6df6e16e2c972b87c`
- 模块/设备树源码提交：`2cc7f4606b65a9ede42030ee82614dd845b665a1`
- 对应的公开源码仓库：
  `android_kernel_oneplus_mt6993` 和
  `android_kernel_modules_and_devicetree_oneplus_mt6993`

仅仅看到相同的 `6.12.58` 不能证明兼容。内核构建后缀、配置、符号 CRC、
结构体布局、KCFI 类型哈希和厂商显示驱动都必须与设备一致。

## 实现原理

模块不修改 DTBO，也不替换面板驱动，而是在运行时挂接 MTK 显示链路：

1. 通过内核符号查找接口定位显示参数、DSI porch 设置和模式枚举函数。
2. 在注册 probe 前检查面板名、分辨率数量、官方档位数量、165Hz 的完整
   H/V 时序和像素时钟。检查失败就返回错误，不继续读写显示结构体。
3. 获取主 DSI 组件。开机早期加载时，在 HWC 第一次枚举前捕获；普通加载
   时则在显示管线第一次运行的 porch 回调中捕获。
4. 在 12 个官方模式后追加 4 个模式：全分辨率/低分辨率 170Hz，再加全
   分辨率/低分辨率 185Hz。新增模式的 DDIC 枚举值映射到已验证的 165Hz
   命令索引，不发送未经验证的新 DCS 命令。
5. `fake_mode=1` 时，所有档位切到统一的 1496Mbps PLL/PHY 链路；官方档位
   按链路比例计算动态 VFP，保持原来的实际刷新率；新增档位使用对应的主机
   像素时钟和 VFP。`fake_mode=0` 时恢复原生 1374Mbps 链路和官方时序。

Android 设置界面显示的数字只是模式枚举结果，不能代替实际刷新率测量。
必须结合 panel/DSI 计数器、示波器或设备端可靠的硬件刷新率接口验证。

## 编译方法

入口只有 `scripts/build_module.sh`。需要以下外部输入：

| 环境变量 | 内容 |
| --- | --- |
| `PMB110_HEADERS_DIR` | 目标设备完整的生成内核头文件树，必须包含 `compiler-version.h`、`kconfig.h`、生成的 UAPI 和 `arch/arm64` 头文件 |
| `PMB110_KERNEL_SOURCE` | 与设备完全匹配的内核源码树，包含 `scripts/module.lds.S` |
| `PMB110_DISPLAY_ROOT` | 匹配模块源码中的 `kernel/kernel_device_modules-6.12` 目录 |
| `PMB110_CLANG` | Android Clang r536225 的 `clang` |
| `PMB110_LD_LLD` | 同一套 r536225 工具链的 `ld.lld` |

示例路径仅作格式说明：

```sh
export PMB110_HEADERS_DIR=/path/to/pmb110-headers
export PMB110_KERNEL_SOURCE=/path/to/android_kernel_oneplus_mt6993
export PMB110_DISPLAY_ROOT=/path/to/android_kernel_modules_and_devicetree_oneplus_mt6993/kernel/kernel_device_modules-6.12
export PMB110_CLANG=/path/to/clang-r536225/bin/clang
export PMB110_LD_LLD=/path/to/clang-r536225/bin/ld.lld
sh scripts/build_module.sh
```

成功产物为 `out/PMB110_185_Mode.ko`，内核内部模块名为 `pmb110_185_mode`，
sysfs 参数路径和运行时控制接口也统一使用该名称。

## ABI、CRC 和 KCFI 注意事项

### vermagic

源码中的 `PMB110_VERMAGIC` 必须与设备 `uname -r` 完全一致，包括构建后缀、
SMP、抢占和 `mod_unload` 标志。不要用“同大版本”的其他内核头编译。

### 符号 CRC

源码包含目标内核的 `__versions`、`__version_ext_crcs` 和符号名表。它们来自
目标内核的 `Module.symvers`。换内核基线后必须重新取得对应 CRC；否则常见
结果是 `disagrees about version of symbol` 或 `Invalid module format`。

### KCFI、PAC 和编译器

构建必须使用 Android Clang r536225（Clang 19.0.1）以及脚本中的 KCFI、PAC
和 AArch64 选项。KCFI 校验间接调用的类型哈希；函数指针 typedef、参数类型、
编译器、优化级别或内核头不一致，都可能让模块在 `do_one_initcall` 或回调
执行时触发 CFI failure。编译能通过不代表加载安全。

### 厂商结构体 ABI

`struct mtk_dsi`、`struct mtk_panel_params`、面板上下文前缀以及 callback
原型是厂商私有 ABI。字段偏移变化通常不会在 C 编译阶段报错，可能只在开机
或切换档位时造成内核崩溃。因此每次换 ROM、内核或显示驱动都必须重新核对
结构体定义、符号地址、模式数量和回调签名。

### 模块签名

如果生产内核启用了强制模块签名，KernelSU/root 权限不能替代内核信任的签名
密钥。未使用设备认可的密钥签名时，模块可能在执行 init 函数前就被拒绝。

## 新增刷新率档位

新增档位必须同时设计“模式时序、像素时钟、链路速率、PHY 参数和 DDIC 路径”，
不能只修改 Android 显示名称。

首先从该面板的官方 DTS、面板驱动和运行时日志记录每个分辨率的
`hdisplay/vdisplay`、H/V sync、H/V porch、像素时钟、lane 数、bpp/DSC 配置、
TE 行为和真实刷新率。固定水平总时序时，目标刷新率近似为：

```text
refresh_hz = pixel_clock_khz * 1000 / (htotal * vtotal)
vfp = vtotal - vdisplay - vsa - vbp
```

实际计算还要遵守驱动的整数单位、最小 VFP、面板 VFP 上限和低功耗刷新限制。
DSC 面板不能直接套用未压缩面板的像素时钟公式，必须按 driver 的 slice、
压缩 bpp 和 DSI 包开销计算链路需求。

验证顺序建议是：先复制已稳定的 165Hz DDIC 路径，只增加一个 host timing；
再逐步调整 VFP、像素时钟和 PLL/PHY；最后扩展 HWC/SurfaceFlinger 模式枚举。
每次只改变一项并记录实际刷新率。所有官方档位都要验证能恢复原生链路，且
逐档正向、反向切换、息屏亮屏、挂起恢复和低亮度场景都要测试。

面板如果是命令模式、内部帧率发生器锁定，或者必须发送特定 DCS 帧率命令，
仅改变主机时序不会真正超频，最多只会改变系统显示的档位名称。

## 其他 MTK 设备参考方法

这份代码不能直接复制到其他 MTK 设备。参考它时应按以下顺序重新做适配：

1. 固定 SoC、内核提交、模块提交和面板型号，先确认 VDO/Command 模式。
2. 从官方 kernel/module 源码取得真实结构体和 callback，而不是凭字段名称
   猜偏移；重新生成目标内核头和符号 CRC。
3. 先实现只读 probe：读取面板名、模式数量、当前 DSI、官方 165Hz 或最高
   稳定档位的完整时序；确认信息正确后再注册 kprobe。
4. 只新增一个测试模式，复用已验证的 DDIC 命令；确认主机确实输出目标刷新率
   后，再引入 PHY/PLL 和动态 VFP。
5. 把面板校验、模式表校验和失败回滚保留在显示写操作之前。
6. 使用该设备自己的 clang、vermagic、KCFI、CRC、`Module.symvers` 和恢复
   方案进行验证。不能因为同属 MTK 或同为 VDO 就共用 `.ko`。

## 其他 Qualcomm 设备参考方法

Qualcomm 的适配思路与本项目的“运行时枚举 + 主机时序 + 链路速率”原则相同，
但驱动 ABI 完全不同，不能复用本项目的 MTK 结构体、符号名或 kprobe 地址。
常见目标是 MDSS/DPU/SDE + DSI 链路，源码通常分布在 `dsi_display.c`、
`dsi_panel.c`、`dsi_ctrl.c`、`dsi_phy.c`、`sde_crtc.c` 以及厂商 Oplus
显示扩展中。

### 1. 先固定 Qualcomm 基线

记录 SoC、Android 版本、完整 `uname -r`、boot/vendor_boot 里的内核版本、
GKI/vendor module 提交、面板节点名（例如 `qcom,mdss_dsi_panel_*`）和 DSI
lane/DSC 配置。把目标设备的 kernel、vendor module、device tree、`Module.symvers`
和 clang 版本作为一个不可混用的 ABI 集合。相同的 Snapdragon 型号或相同的
6.x 内核版本，不足以证明模块兼容。

### 2. 找到官方模式和时序来源

重点检查面板 DTS/DTBO 和面板驱动中的：

- `qcom,mdss-dsi-panel-mode`、`qcom,mdss-dsi-mode`
- `display-timings`、`qcom,mdss-dsi-timing`
- `h-active`、`h-front-porch`、`h-sync-width`、`h-back-porch`
- `v-active`、`v-front-porch`、`v-sync-width`、`v-back-porch`
- `bit-clock-rate`、`clk-rate`、`phy-timing`、`mdp-transfer-time-us`
- DSC slice 数、slice width、compressed bpp 和命令模式/视频模式标志

运行时还要确认 DSI display 的 mode list、SDE/HWC 的 mode ID 和 SurfaceFlinger
最终使用的 mode。系统界面显示的数字不是硬件测量值。

### 3. 判断面板能否跟随主机时序

VDO 面板通常由主机持续发送视频流，理论上可通过 H/V porch、像素时钟和 DSI
bit clock 提高真实刷新率；但仍需确认 DDIC 没有内部刷新率上限。Command mode
面板往往需要特定 DCS 切换命令和 TE 配置，不能只改 host timing。先用官方最高
档位作为 DDIC 基准，禁止盲目发送自造的 DCS payload。

### 4. 计算 Qualcomm 时序和链路

使用和 MTK 相同的总时序公式：

```text
refresh_hz = pixel_clock_khz * 1000 / (htotal * vtotal)
vfp = vtotal - vactive - vsa - vbp
```

Qualcomm 驱动中可能把时钟存为 Hz、kHz 或 bit clock；必须先确认单位和 lane
数。对 DSC 面板，链路需求由压缩后的 bpp、slice 配置和 DSI 包开销决定，不能
直接按 RGB 原始 bpp 计算。提高 bit clock 后要同步检查 DSI PHY timing、PLL lock
范围、escape clock、HS prepare/trail、CLK prepare/trail、TA 等参数以及 panel
时序约束。只改 `vfp` 而不改变 `bit_clock_rate`，通常只能降低实际刷新率周期，
无法让链路传输能力超过原上限。

### 5. 选择实现层

优先级一般是：

1. 能修改并重新签名 DTBO/内核时，直接在官方 mode table 增加完整的 timing、
   clock、PHY 和 panel command 配置，并同步 HWC/SDE 的 mode 枚举。
2. 只能加载 LKM 时，先寻找稳定且导出的 Qualcomm 显示回调；必要时使用
   kprobe/kretprobe，但必须验证函数原型和 KCFI 类型。运行时复制官方 mode
   结构、增加 mode list，并在 DSI mode switch callback 中选择匹配的 timing。
3. 如果 HWC 在模块加载前缓存了 mode list，应在显示枚举前早期加载，或在确认
   缓存失效机制后重新触发一次安全枚举；不要无依据地重启 SurfaceFlinger。

Qualcomm 的 `dsi_display_mode`、`dsi_mode_priv_info`、`dsi_panel_mode`、SDE
connector 私有数据和 Oplus 扩展字段必须从对应版本源码取得。不要把本项目的
`pmb110_lcm_prefix`、MTK `mtk_dsi` 或 mode ID 直接移植过去。

### 6. Qualcomm 的 ABI/CRC/KCFI 风险

- vermagic 必须匹配完整 Qualcomm kernel release 和配置。
- `CONFIG_MODVERSIONS` 打开的内核必须使用同一份 `Module.symvers` 生成 CRC。
- `CONFIG_CFI_CLANG`/KCFI、LTO、PAC/BTI、编译器版本和内核头必须与目标 build
  保持一致；函数指针签名变化会在间接调用时触发 CFI failure。
- `struct dsi_display`、`struct dsi_panel`, `struct dsi_display_mode`、
  `struct dsi_display_mode_priv_info`、SDE connector 私有结构都是非稳定 ABI，
  不能依赖另一个 Snapdragon 型号的偏移。
- Qualcomm vendor symbols 可能未导出，`kallsyms_lookup_name`、kprobe 黑名单、
  SELinux 和模块签名策略都可能阻止注入；必须先做只读探测并准备恢复方式。

### 7. Qualcomm 验证顺序

先验证模块能在 `fake_mode=0` 下加载且不注册写操作，再验证 panel 名称和官方
mode 数量；之后只添加一个低风险测试档位，记录 DSI bit clock、pixel clock、
H/V total、TE 和实际刷新率。每次修改后都测试官方档位、息屏亮屏、挂起恢复、
亮度变化和所有方向的档位切换。出现花屏、白屏、黑屏、无限重启或 DSI timeout
时，立即停用模块并保存 pstore、dmesg、logcat、HWC/SF mode 列表和 PHY/PLL
寄存器状态，不要继续提高时钟掩盖根因。

## 安全边界

仓库中的实现只验证了 PMB110 基线。高刷新率可能超出面板 DDIC、DSI PHY、触控
同步、功耗或温度设计范围；即使模式枚举成功，也不代表长期稳定或硬件安全。
其他设备只能参考本文档建立自己的源码、ABI、校验和恢复流程，不能直接复用
本项目生成的 `.ko`。
