# VTB Qt 上位机

在 Mac 上通过已有的 `rk3588-direct` SSH 别名管理 RK3588 的
`vtb-live2d.service`。本目录可在 macOS 独立构建，依赖 CMake 3.21、C++20、
Qt 6.4 以上的 Widgets 和 Network、FFmpeg 开发库、OpenSSL 和 pkg-config。
首次配置通过 CMake 拉取固定版本的 libdatachannel 及其子模块。

## 构建与运行

从 VTB 仓库根目录执行：

```sh
cmake -S apps/vtb-qt -B build/vtb-qt -DCMAKE_BUILD_TYPE=Release
cmake --build build/vtb-qt --parallel
open build/vtb-qt/vtb-qt.app
```

Qt 不在 CMake 的搜索路径中时，将 `-DCMAKE_PREFIX_PATH` 指向 Qt 安装前缀。
直接运行演示模式可体验界面、明暗主题和操作状态。演示模式使用模拟板端数据。

```sh
build/vtb-qt/vtb-qt.app/Contents/MacOS/vtb-qt --demo
```

## 板端连接

- 普通模式按固定间隔经 SSH 只读回查系统服务、配置和输出状态。视频预览页
  以 Mac 实际收到视频帧为准，板端端口和 USB 状态用于诊断。
- 用户点击启动、停止、保存、应用并重启或查看日志时，应用使用板端现有的
  非交互 `sudo` 权限执行内置 Python 命令。RK3588 需要 `systemctl`、
  `journalctl`、`ss` 和 Python 3。Mac 端使用原有 SSH 主机指纹及身份配置。
- 配置路径由 `/etc/vtb/live2d-uvc.env` 的 `VTB_LIVE2D_CONFIG` 决定。
  保存前会对照读取时的 SHA-256，检测其他进程对文件的修改，再在同目录原子替换。
  停机时保存，下一次启动生效。运行时应用需要确认重启，UVC 和 RTSP 会短暂中断。
- 播放器端口 8080 和信令端口 8443 由当前 Live2D 板端程序固定。
- 上位机关闭后，Live2D 继续由 systemd 管理。

## 视频预览

从侧栏「视频预览」选择 UVC、RTSP 或 WebRTC，画面显示在同一个主区域。
切换到其他页面时释放当前预览。

- **UVC** 通过 AVFoundation 打开 Mac 已识别的 USB 摄像头。需连接 RK3588 的
  UVC 数据线，选择对应设备并允许 macOS 相机访问。板卡未枚举时可查看等待提示。
- **RTSP** 通过 FFmpeg 以 RTSP over TCP 接收并解码配置中的视频流。
- **WebRTC** 使用板端现有 WebSocket 信令和 libdatachannel 接收 H.264，再由
  FFmpeg 解码。预览页使用板端信令端口 8443。

各页在收到第一帧后显示「播放中」。可在当前预览页使用「重新连接」。
布局参见本机 Penpot 的 [04 · 视频预览](http://localhost:9001/#/workspace?team-id=568a29fc-d1ae-80e2-8008-cb2465e0be85&file-id=965777a7-3802-8037-8008-f7244ff649c1&page-id=99051690-da38-80f2-8008-f83a31e119d5)。

## 板卡监控

在编译服务器上使用适配目标板的 Linux ARM64 交叉工具链构建采集器。
本板的 5.10 BSP 使用 Arm GNU Toolchain 12.2.Rel1 构建此独立目标。

```sh
cmake -S apps/vtb-qt/board/monitor -B build/vtb-board-monitor \
  -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_CXX_COMPILER="$ARM_GNU_ROOT/bin/aarch64-none-linux-gnu-g++" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/vtb-board-monitor --parallel
```

从 Mac 取回并校验产物后，将可执行文件部署到 `rk3588-direct` 上的
`~/VTB/runtime/vtb-board-monitor`，并确认板端二进制校验值。
启动上位机时，通过普通用户的 SSH 会话启动采集器。
「概览」约每秒更新 CPU 总占用和逐核占用、CPU 频率、内存、温度、GPU 负载与频率及 NPU 频率。
上位机启动采集器时传入 `--details`，约每两秒更新 Live2D 主进程及全进程列表、
网络接口收发速率和物理磁盘读写速率。Live2D 主进程由 systemd 的 `MainPID` 确定，
进程 CPU 以单核 100% 为单位，内存显示 RSS。速率页可按接口或磁盘查看最近 60 秒。
CPU 占用和速率在取得两次有效采样后可用。旧版采集器可继续提供基础监控。
上位机运行期间持续采样，切换侧栏页面或最小化后仍会更新。
退出上位机时关闭采样会话。断线时会标记旧数据并重试。
`--demo` 使用示例读数，适合离线查看页面。

## 离线检查

`python3 -B -m unittest discover -s apps/vtb-qt/tests` 验证配置校验、
原子保存、重启失败回滚和状态读取，使用本机临时文件与模拟服务。
