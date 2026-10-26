# 构建维护

外部源码版本与校验信息集中在 `manifests/kernel.json`、`manifests/openwrt.json` 及 `integration/openwrt/feeds.conf`。当前基线为 Linux v7.3 与 OpenWrt v25.12.5。

## 构建树初始化

`tools/bootstrap.py` 负责准备 OpenWrt 构建树：
1. 浅拉取 `manifests/openwrt.json` 锁定的 OpenWrt 官方提交；
2. 按 `integration/openwrt/series` 顺序应用板级补丁（Linux 7.3 兼容、target 内核版本支持、`procd` 状态修复及 `sysupgrade` 板级钩子）；
3. 同步 `integration/openwrt/target/` 与本地 `packages/` feed。

重复运行 `bootstrap.py` 会复用已有 `out/openwrt-build`，仅同步 target 与内核清单，保留已编译工具链与包缓存。

## 更新内核源码锁定

本仓库内的 `components/` 与 `packages/` 直接由本地源码树参与构建；`linux-udx710` 内核源码则通过 `manifests/kernel.json` 锁定仓库地址与提交哈希。更新内核时，保持 `manifests/kernel.json` 中的 `version`、上游标签和提交、本地合并提交、归档名及 SHA-256 一致；导出目录和 OpenWrt 内核版本由 `version` 派生。`tools/build/kernel-source.py` 导出锁定提交并校验归档，同时更新构建树的 `LINUX_KERNEL_HASH`。

Linux v7.3 的合并以官方 v7.3-rc7 为三方合并基线。仓库原先导入的 rc7 根提交与该官方提交的 Git tree 完全一致，合并保留本地系列并建立正式上游祖先关系。AUDCP 实验接入不在当前内核树中；MCDT 的 ROC1 适配及 Binding 单独保留。

## 单树双配置构建

`firmware.py main` 与 `firmware.py recovery` 共用 `out/openwrt-build` 构建树（工具链、feeds 与下载缓存共享），需串行执行。构建完成后，产物分别输出至 `out/images/main` 与 `out/images/recovery`。每套产物的 `build.json` 记录实际构建使用的 `integration_commit` 和内核锁定提交；后续文档修订独立于已构建镜像版本。

## 独立启动器

`sy108-uboot-spl` 启动器在该独立仓库自身的 Linux AMD64 Docker 环境中构建并完成 QEMU/Unicorn 离线仿真校验：

```sh
make -C /path/to/sy108-uboot-spl offline LABEL=mainline \
  SPL_IMAGE=/absolute/path/to/original-spl.bin \
  SML_IMAGE=/absolute/path/to/original-sml.bin \
  MAIN_CAPSULE=/absolute/path/to/sy108-mainline/out/images/main/boot.bin \
  RECOVERY_CAPSULE=/absolute/path/to/sy108-mainline/out/images/recovery/boot.bin
```

输出产物为 `sy108-uboot-spl/build/mainline/uboot.bin` 及离线验证报告 `offline-report.json`。


## LuCI 扩展

官方 LuCI feed 不再应用板级源码补丁。`luci-app-sy108-system` 在 Main 和 Recovery 中提供独立的系统维护视图，通过 `menu.d` 注册入口并将原升级、重启菜单路由到该视图。备份及镜像校验界面基于锁定的官方 LuCI 实现，升级参数和强制升级选项保持其原有行为；产品重启、恢复配置和恢复出厂动作通过现有平台后端执行。

## 交付检查

从干净提交构建 Main、Recovery，保留配置、源码锁定和镜像 SHA-256 对应关系。Main 候选通过启动、CPNV/HALO/NV、双栈、USB 和正常停止验收后，才更新 Recovery；Recovery 写入后整卷读回，并验证 Main → Recovery → Main，最终停留 Main。

设备树验证包括仓库 Binding 与原始 chosen schema、[本地启动 ABI](devicetree.md)、解析引用后的资源比较及现有 U-Boot 离线解包。原始 schema 日志保留既有启动属性差异，不能将本地扩展校验描述为原样上游零警告。
