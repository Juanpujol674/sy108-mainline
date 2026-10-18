# CP/NV 基带运行服务

本目录实现 UDX710 原生 CP 固件装载、HALO 认证握手、原厂 UBI NV 镜像编解码与持久化写回、以及 `refnotify` 通知服务，由 `packages/udx-cpnv` 调用 `build.sh` 编译打包。
