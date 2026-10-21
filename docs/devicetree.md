# 设备树与启动 ABI

SY108 设备树位于内核 `arch/arm64/boot/dts/sprd/`，Main 与 Recovery 共享硬件描述，通过各自 DTS 选择服务和根文件系统。

| 文件 | 归属 |
| --- | --- |
| `udx710.dtsi` | SoC 控制器、地址、时钟、DMA、IRQ、ADI、PM_SYS 和片上传感器 |
| `sc2730.dtsi` | 通用 PMIC 内部设备、寄存器与校准 cell，嵌入板级创建的 PMIC 节点 |
| `udx710-sy108.dtsi` | 板级连接、PMIC 放置和电源、NAND 分区、热管理、MCU、外部时钟与 CPU 策略 |
| `udx710-sy108-pinctrl.dtsi` | 保留控制器所引用的板级 pinctrl 状态 |
| `udx710-sy108-firmware.dtsi` | 固件保留内存、装载窗口和 SIPC 缓冲布局 |
| `udx710-sy108.dts` | Main 的直接 UBI 根和 PAMU3 USB offload |
| `udx710-sy108-recovery.dts` | Recovery 的 RAM 根及禁用服务差异 |

小型硬件片段直接归入对应层。SoC 从板级获得 `ext_26m`、`ext_32k` 输入，CPU 的 `cpufreq-data-v1` 由板级连接至既有策略节点。

源码使用节点引用，由 dtc 分配 phandle。比较时解析引用目标的节点路径，并保持资源、参数和数组顺序；编译生成的 phandle 整数和 DTB 字节不作为兼容性依据。GPIO、efuse、dump 的引用也使用节点引用。dump 列表的第三项保留为空槽，不为它补入未确认的内存区域。

停用的手机音频、耳机及逻辑 MBIM 前端已移除。真实 SoC 控制器、仍有消费者的固件内存、`cpufreq-data-v1` 和 dump 接口保持。pinctrl 仅移除在 Main、Recovery 中均无引用的配置组；有引用的组及其子配置保留。

## 保留的启动属性

这些属性属于已安装启动器与 Linux 的既有协议，位于 `/chosen`。

| 属性 | 类型 | 约束 |
| --- | --- | --- |
| `udx,boot-policy` | 字符串 | `openwrt-v1` |
| `udx,boot-target` | 字符串 | Main 为 `openwrt`，Recovery 为 `recovery` |
| `udx,trial-stage-schema` | 单个 uint32 | `6` |
| `udx,trial-rtc-stages` | 空布尔属性 | 两个配置均存在 |
| `udx,trial-local-lease` | 空布尔属性 | 两个配置均存在，保留启动器标记含义 |
| `udx,recovery-ram` | 空布尔属性 | 仅 Recovery 存在 |

`tools/lib/boot_layout.py` 的 `boot_abi()` 校验类型、取值和目标关系，镜像打包和独立 DTB 检查共用该实现：

```sh
tools/python tools/check/dt-boot-abi.py --profile main out/images/main/board.dtb
tools/python tools/check/dt-boot-abi.py --profile recovery out/images/recovery/board.dtb
```

## Binding 校验范围

内核不携带完整的 chosen schema 副本，校验直接使用 dt-schema 原始 `chosen.yaml`。标准 schema 不接受上述私有属性；保留这些属性会产生原始诊断，独立 ABI 校验不能把这些差异变成上游认可的属性。

硬件校验使用 dt-schema 内置 schema 与本仓库的硬件及 CPU Binding。`cpufreq-data-v1` 属于保留的下游 CPU 扩展，因此整个结果不能称为原样上游零警告。

当前两套源码 DTB 的结果为：

| 配置 | 原始 schema 诊断 | 禁用节点 | 既有启动属性相关 | 其他启用节点 |
| --- | ---: | ---: | ---: | ---: |
| Main | 254 | 249 | 5 | 0 |
| Recovery | 254 | 249 | 5 | 0 |

启动属性的五条诊断包含三条类型推断诊断，以及 chosen 和厂商前缀各一条合并的未知属性诊断，涉及上表明确列出的属性。`udx` 是保留接口的命名空间，不作为硬件厂商登记；现用 MCU 的 `meig` 厂商前缀单独注册。只有这些属性通过独立 ABI 校验后，才能归类为保留协议差异。其余启用节点要求零诊断；不使用整节点或整 schema 忽略。

禁用节点的原始日志、结构化诊断及逐节点清单随验证证据保存，数量按实际 DTB 重新统计。dtc 的剩余节点警告也仅涉及禁用节点。构建后的 Main、Recovery DTB 还需重复校验、检查资源语义并用现有启动器完成离线解包；源码校验不能代替实机启动和外设验收。
