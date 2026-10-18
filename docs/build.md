# 构建维护

外部源码版本与校验信息集中在 `manifests/kernel.json`、`manifests/openwrt.json` 及 `integration/openwrt/feeds.conf`。当前基线为 Linux v7.3-rc7 与 OpenWrt v25.12.5。

## 构建树初始化

`tools/bootstrap.py` 负责准备 OpenWrt 构建树：
1. 浅拉取 `manifests/openwrt.json` 锁定的 OpenWrt 官方提交；
2. 按 `integration/openwrt/series` 顺序应用板级补丁（Linux 7.3 兼容、target 内核版本支持、`procd` 状态修复及 `sysupgrade` 板级钩子）；
3. 同步 `integration/openwrt/target/` 与本地 `packages/` feed。

重复运行 `bootstrap.py` 会复用已有 `out/openwrt-build`，仅同步 target 与内核清单，保留已编译工具链与包缓存。

## 更新内核源码锁定

本仓库内的 `components/` 与 `packages/` 直接由本地源码树参与构建；`linux-udx710` 内核源码则通过 `manifests/kernel.json` 锁定仓库地址与提交哈希。当 `linux-udx710` 提交更新后，只需更新 `manifests/kernel.json` 中的 `commit`（或留空默认使用 `HEAD`），`tools/build/kernel-source.py` 会在构建时自动导出归档包并计算更新 `LINUX_KERNEL_HASH`。

## 单树双配置构建

`firmware.py main` 与 `firmware.py recovery` 共用 `out/openwrt-build` 构建树（工具链、feeds 与下载缓存共享），需串行执行。构建完成后，产物分别输出至 `out/images/main` 与 `out/images/recovery`。

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

