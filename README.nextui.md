# wiliwili-nextui

[wiliwili](https://github.com/xfangfang/wiliwili)（B 站第三方客户端）适配 **NextUI** 运行于
**TrimUI Brick Pro / tg5040**（Allwinner A133P，1024×768，TinaLinux 4.9，aarch64，glibc 2.33，
PowerVR GE8300 / GLES 3.2，SDL2 手柄输入）的分支。

## 来源与许可

- 上游：`xfangfang/wiliwili` **v1.6.0**（commit 见上游 tag），**GPLv3**（见 `LICENSE`）。
- 本仓库是上游 v1.6.0 的**自包含源码快照**：原 git 子模块（borealis、cpr、mongoose、OpenCC、
  lunasvg、pystring、QR-Code-generator、libpdr、MemoryModule）已展开为普通目录随仓库携带，
  不再依赖上游子模块 URL。各子模块保留其原始许可证。
- **不使用社区预编译包**（如 dragonflylee/trimui-port 的产物，其 ffmpeg 含 `--enable-nonfree`
  不可再分发）。本仓库从源码自建，参考社区构建思路但产物自行构建、许可证自行处理。

## 适配目标（详见 NEXTUI.md）

- 图形：`-DUSE_GLES3=ON`（GE8300 支持 GLES 3.2；GLES2 无法跑视频，libplacebo 要求 GL≥3）。
- 视频：libmpv + FFmpeg **软解**（BSP 无 ffmpeg 可用的硬解通道）；目标 480p 稳、720p30 可用。
- 输入：SDL GameController（设备 js0 报 "Xbox 360 Controller"）；无屏幕键盘，扫码登录可用。
- 集成：NextUI pak（`Tools/tg5040/WiliWili.pak`），HTTPS 指向系统 CA bundle，运行期防休眠，
  HOME 退出由系统全局浮层兜底（无需应用内 overlay 适配）。
- 构建：NextUI tg5040 工具链（`ghcr.io/loveretro/tg5040-toolchain`，CI/Docker 主机），
  自建 mpv/ffmpeg/libass 等依赖闭包，去除 nonfree 以可分发。

## 状态

适配进行中。构建与打包说明见 `NEXTUI.md`；上游原始文档见 `README.md` 上游内容与 `docs/`。
