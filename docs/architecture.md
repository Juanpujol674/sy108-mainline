# 架构与接口

`sy108-mainline` 维护 OpenWrt 集成、用户态服务和镜像工具，`linux-udx710` 维护硬件驱动与设备树，`sy108-uboot-spl` 维护启动器。硬件资源与设备生命周期由 Linux 管理；procd、ubus、UCI、netifd、sysupgrade 和 LuCI 使用 OpenWrt 的标准接口。

## 启动与存储

原厂 SPL、DDR 初始化及 SML 保持。U-Boot proper 根据 `misc` 状态和 SC2730 RTC 阶段选择 Main 或 Recovery，解析 UBI 卷中的数据 Capsule，并在试启动失败时回退 Recovery。Capsule 格式及校验代码位于 `components/boot-format/` 和 `tools/lib/boot_layout.py`。

Main 直接挂载 UBI `rootfs` 卷的 SquashFS，通过标准 preinit、fstools、procd 启动，可写配置位于 `rootfs_data`。Recovery 使用独立 initramfs 和 RAM 根，只提供软件 NCM、平台监控和基本管理服务，不启动 modem、NV 或蜂窝服务。Recovery 可只读访问 Main 配置以保留配置升级。

启动协议保留现有 misc 状态、RTC 编码和六个 `/chosen` 属性。设备树组织、Binding 校验范围及这些属性的类型约束见 [设备树与启动 ABI](devicetree.md)。

## 运行时与启动确认

`udx-runtime` 的单个 libubus/uloop 事件循环提供 `udx.runtime` 对象，执行健康检查、试启动期限及 PMIC 看门狗续喂。方法包括 `status`、`checkpoint`、`reboot(target)`、`prepare_upgrade`、`stop` 和 `mcu_poweroff`；修改方法仅供本机 root 使用。

APCPU `watchdog0` 由 procd/upgraded 管理，SC2730 `watchdog1` 由 runtime 独占。PMIC 看门狗为 nowayout，正常以 20 秒周期续喂 60 秒计时器；显式重启在收尾后写 RTC 原因和阶段，再设置既有的 12 秒窗口，升级使用 60 秒窗口。进入终止状态后不恢复普通续喂。

`udx-boot-state management-ready` 检查实际 UDC、管理监听、netifd 管理地址、procd 服务及 dnsmasq 子进程实际可见的配置。dnsmasq PID 来自 procd 的 jail `pidfile`，配置从该进程的 `/proc/<pid>/root` 读取。零 DHCP 请求及 Recovery 的 DNS `port=0` 有效。管理与核心依赖就绪后，启动确认服务将 misc 事务提交为 ready。无 SIM、基带 degraded 或无公网不作为 core 启动失败条件。

本地 `udx-runtime --status/--checkpoint/--reboot/--prepare-upgrade/--stop/--mcu-poweroff` 命令保留，LuCI 经只读设备 RPC 查询运行状态。

## CPNV

procd 服务 `udx-cpnv` 直接启动一个 `udx-cpnvd`。正常运行时只有主线程和一个 NV 工作线程。

| 所有者 | 职责 |
| --- | --- |
| 主线程 | ubus、基带状态流、HALO 状态机、refnotify、订阅连接和定时器 |
| NV 工作线程 | 固件材料准备、一次同步 remoteproc 启动、NV 读取、暂存、提交及其 fd 和缓存 |

线程间使用任务状态、短时互斥和 eventfd 通知。工作线程不调用 ubus，主线程不关闭工作线程的设备 fd。启动期间可以查询状态；固件准备、Alive、HALO、NV 就绪和后续监控分别推进。

CP/DSP 固件从设备原厂 UBI 卷提取，九段材料暂存于 `/run/udx-cpnv-warm/*.bin`。`/lib/firmware/sprd/udx710-cp` 指向这个 RAM 目录。服务确认 modem remoteproc 为 offline 后准备材料，只写一次标准 `state=start`；同步加载完成后删除材料。容器解析、NV 格式和 checksum 属于用户态，实际内存装载窗口及处理器释放顺序属于内核。modem remoteproc 禁用自动恢复和独立热停止。

HALO 的 RSA 与摘要直接使用 OpenSSL，保留协议字序、编码和运算参数。特征应答完整消费 152 字节，其中 `9..92` 为特征记录。私钥操作结束后释放密钥上下文并清除临时私钥字节。平台材料位于 `components/cpnv/halo/`，安装至 `/etc/udx-cpnv-warm/`，权限 `0600`。设备固件、NV、校准和 IMEI 保存在设备本地，不随镜像分发。

NV 使用原厂格式、checksum 宽度、双副本选择及 group/sequence。提交按原有规则先更新备份并同步、检查备份头 checksum，再更新主副本并同步；完成后才发送最终 36 字节 ACK。ACK 写入成功表示本地 transport 接收，不能证明 CP 已收到。分块 RAM 缓存使用系统 liblz4。

### CPNV 状态与停止

`udx.cpnv` 只提供 `status`、`quiesce`。状态包括 `starting`、`running`、`failed`、`quiescing`、`quiesced`，保留以下字段：

- `ready`、`nv_ready`、`halo_ready`、`storage`；
- `accepting`、`nv_committing`、`uncommitted_discarded`、`last_error`。

查询读取主循环快照，不等待存储 I/O。`quiesce` 立即停止接收新事务，丢弃尚未提交的 RAM 数据；已开始的提交继续完成原有规则和最终 ACK。工作线程关闭设备并报告退出后，主线程才发布收尾完成。

停止请求最多等待 10 秒，超时返回失败并保持停止接收。服务不恢复接收、不重载 CP，也不用 `pthread_cancel` 打断写入。已完成的重复停止成功，进行中的重复请求不重新启动流程。procd 的终止等待覆盖原有 60 秒 NV 事务预算。收尾成功表示用户态事务和 fd 已结束，不表示 CP 硬件停止。

### 时间通知订阅

refnotify 与 HALO 共用主事件循环，只服务只读 RTC 查询和 CP 时间关联。它不设置 RTC 或系统时钟。抽象 Unix 流 socket `cp_time_sync_server` 保持八字节记录、root 身份检查和三个客户端限制；新订阅者接收最近记录。旧记录尚未发完时收到新记录的慢客户端会断开，防止破坏记录边界。

## 蜂窝与网络

`udx-cellulard` 的 uloop 主线程提供 `udx.cellular`，一个 AT 工作线程串行执行控制、URC 和后台采集。UCI 使用 libuci，摘要使用 OpenSSL。

`status` 返回带采集时间和有效性信息的内存快照。控制操作通过各自的 ubus 方法提交，返回 `operation_id`、`boot_id`、`broker_id`，再由 `operation_status` 查询。一次只执行一项控制操作，最近 32 项结果保存在内存。短信持久去重及结果不确定时禁止重发的规则保持。

`network_setup`、`network_teardown`、`quiesce` 供本机系统服务使用。netifd 的 `udx` 协议使用 `wan` / `wwan0`，服务异步推送完整配置；DHCPv6 由 netifd/odhcp6c 管理。撤销接口时先确认地址、路由已清除，再执行 AT/PDP 收尾。ubus 重连后重新注册并请求协议 renew。

内核提供两个标准 WWAN AT 端口，分别用于 URC 与命令；数据链路使用 SIPA WWAN。SPIPE 保留 `/dev/spipe_lte2`、`spipe_lte3`、`spipe_lte8` 和 `/dev/snv_lte` 流接口，已打开的 fd 始终绑定其内核实例。有效的 `sipa-dele/cp_session` 仍提供 CP 会话信息。

## 平台外设

PM_SYS preinit 从设备 `nr_pmsys` 卷准备 RAM 固件，再加载标准 remoteproc 驱动。SIPC 建立邮箱接收端后启动 CM4；SP UART、MCU 和 DDR 等待该供应者正常 probe。PM_SYS 状态通过标准 remoteproc `state` 查询。

MCU 使用 SP UART serdev，IIO 暴露 ADC3..5。USB 事件经一次协议 ACK 和 uevent 交给既有 gadget 服务串行重连；电源事件经 runtime 和 procd 有序关机，电源 ACK 位于 `SYS_POWER_OFF`。协议和连接说明见 [USB 与 MCU 接口](usb-mcu.md)。

产品通过标准 devfreq 读取 DDR 状态，通过 cpufreq 管理 CPU。Orca NVMEM 保留校准 cell，并提供 `fixed-layout` UID cell；蜂窝状态中的 UID 暂不可用。语音实验模块不进入正式配置。

## 升级与维护

`luci-app-sy108-system` 提供 Main/Recovery 独立维护视图，经 `menu.d` 接入。官方 `luci-mod-system` 的 flash/reboot 源码保持原样。

升级和配置重置先调用 `udx-system-quiesce`，依次检查 cellular、CPNV 的返回码，失败立即退出。成功后才写 misc pending、申请升级窗口并进入标准 sysupgrade stage2。USB 保持到 stage2，由 procd 切入 RAM 环境后更新 Main 的 boot/rootfs 卷并保存配置。

正常停止按 cellular（12）、CPNV（15）、USB（20）、runtime 的顺序执行。普通关机的收尾失败记录日志后继续原有停止流程。Recovery 镜像单独维护；交付顺序为先升级并验收 Main，再写 Recovery、整卷读回并验证 Main → Recovery → Main。
