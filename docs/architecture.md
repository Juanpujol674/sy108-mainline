# 架构与接口

`sy108-mainline` 维护用户态核心服务、OpenWrt target/feed 集成与构建脚本；`linux-udx710` 维护内核驱动与设备树；`sy108-uboot-spl` 维护 SPL 与 U-Boot 启动器。

## 启动链路与存储布局

主系统直接挂载 UBI 上的 SquashFS `rootfs` 卷，使用 OpenWrt 标准 `preinit`、`fstools` 与 `procd`，可写 overlay 位于 `rootfs_data` 卷。独立 Recovery 采用 OpenWrt `initramfs` 镜像与 RAM 根文件系统，仅启动软件 NCM 与基础管理服务（不加载基带固件与 CP/NV 服务）。Recovery 在需要保留配置升级时可只读挂载主系统配置卷。

U-Boot proper 由 `sy108-uboot-spl` 仓库构建，配合原厂 SPL、DDR 初始化与 SML 运行。U-Boot 直接解析 `boot` / `recovery` 卷中的 Capsule 镜像，根据 `misc` 分区状态与 RTC 启动阶段寄存器选择启动主系统或 Recovery，并在试启动超次或异常时自动回退至 Recovery。Capsule 格式定义与用户态校验实现位于 `components/boot-format/`。

启动阶段与 PMIC 保活租约通过 SC2730 RTC 备份寄存器与设备树 `udx,trial-local-lease` 属性协同工作：
- U-Boot 写入初始启动阶段；
- 内核早期与 RTC 驱动在系统初始化期间维护短期保活租约；
- 用户态运行管理器（`udx-runtime`）接管后续健康检查与租约续期，启动确认服务在管理链路就绪后将 `misc` 状态提交为 `ready`。

## CP/NV 与蜂窝服务

- **PM_SYS 与 Modem 生命周期**：PM_SYS 固件从原厂 UBI 卷读取，在 SIPC 接收端就绪后装载启动；CP 与 DSP 固件同样从原厂 UBI 固定卷加载。原生服务（`udx-cpnv`）负责基带启动、HALO 握手与原厂 NV 镜像读写，关机或升级前按序收尾 AT 会话、刷回 NV 并释放基带资源。
- **HALO 参数**：平台通用 HALO 参数位于 `components/cpnv/halo/`，随 `udx-cpnv` 安装至 `/etc/udx-cpnv-warm/`。设备专属基带固件、NV、射频校准及 IMEI/序列号保存在设备本地闪存中，不包含在固件镜像内。
- **AT 与网络管理**：`udx-cellular` 通过单一控制通道串行化后台状态轮询与手动 AT/短信操作，并通过 `rpcd` 与 `netifd` 向上层提供接口。

## 升级与恢复

固件升级包遵循 OpenWrt `sysupgrade-tar | append-metadata` 规范，使用 `fwtool` 校验设备型号与整包 CRC。

- **主系统升级**：先停止 CP/NV 服务并断开 USB Gadget，随后由 `procd` 切换至 RAM 根环境，调用 `ubiupdatevol` 更新 `boot` 与 `rootfs` 卷，通过 `nand_restore_config` 保存配置归档，并将 `misc` 启动事务置为新固件试启动（`pending` / `trial`）。
- **自动回退与恢复**：新主系统首次启动受试启动租约保护；若在期限内未能完成启动确认，看门狗复位后 U-Boot 会自动进入独立 Recovery，由 Recovery 重新刷写主系统固件。
