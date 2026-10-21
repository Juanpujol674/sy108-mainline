# USB 与 MCU 接口

SY108 使用 UDX710 USB1（`0x29100000`）提供管理网络。内口 Type-C 具备 SS 与 HS 引脚，外口 Type-C 提供 HS 与供电；板级 MCU 管理 CC、供电及部分 HS 路由。USB0 保持禁用。

Main 的 NCM 使用 PAMU3/SIPA 硬件路径，Recovery 使用软件 NCM。`udx-usbctl` 负责 gadget 配置和串行重连；切换 NCM 处理方式通过正常重启完成。MCU 的 USB 事件复用该服务的 apply 和控制锁。

VBUS 接入由 SC2730 EIC0 的 `extcon_usb0` 提供。板级 SC2730 Type-C/PD 节点禁用，外部 CC 路由由 MCU 的既有硬件策略管理。

## MCU 连接

板载 CS32G020 经 SP UART serdev 通信，115200 8N1，串口编号 `ttyS1`。SP UART 使用 PM_SYS transport，等待其 remoteproc/SIPC 供应者就绪。GPIO70、GPIO71 分别连接 MCU reset、wakeup，驱动通过标准 GPIO descriptor 获取。

## 串口帧

| 偏移 | 长度 | 含义 |
| ---: | ---: | --- |
| 0 | 1 | `0xAA` |
| 1 | 1 | Major，普通通信为 `0x60` |
| 2..3 | 2 | 小端总长，`payload_len + 7` |
| 4 | 1 | Minor；应答/ACK 为请求 Minor 的高低半字节交换 |
| 5 | 1 | payload 长度 N |
| 6..6+N-1 | N | 线上顺序的 payload |
| 6+N | 1 | 前面所有字节累加和低 8 位 |

当前驱动接口：

| 功能 | 请求 / 应答 Minor | 行为 |
| --- | --- | --- |
| ADC | `01` / `10` | 请求 `[channel, 0, 0, 0]`，应答回显通道，值为第 2..3 字节的小端 uint16 |
| 固件版本 | `06` / `60` | 请求无 payload，应答为 64 字节版本字符串 |
| MCU 请求 AP 关机 | `04` / `40` | 保存请求，经 runtime/procd 有序关机，在 `SYS_POWER_OFF` 应答零长度 ACK |
| USB 重连事件 | `0A` / `A0` | 回复 `[0]` ACK 后发布 uevent，gadget 服务断开、等待 500 ms、重连 |

ADC3..5 通过 IIO `in_voltage3_raw`、`in_voltage4_raw`、`in_voltage5_raw` 暴露 MCU 原始采样值，单位换算由调用方处理。固件版本通过 `firmware_version` 读取。

电源事件可以早于用户态到达，驱动保留待处理请求。电源 ACK 仅在确有请求且系统进入关机阶段时发送，位于 UART device shutdown 之前；普通 restart 不发送该 ACK。USB ACK 在驱动内完成一次，用户态接收 uevent 后排队执行重连。

MCU GPIO、休眠、Nearby、主动断电、Modem 供电及 MCU 固件升级没有正式用户接口。UART transport 为内建设备，不提供独立热重载或驱动解绑操作。
