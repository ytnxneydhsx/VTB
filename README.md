# RK3588 USB 摄像头示例与 RTSP 推流

Mac 与 RK3588 的直连调试、上网优先级和仓库同步脚本见 [`network/README.md`](network/README.md)。

## C++ 程序

- `src/main.cpp`：新的 VTB 主程序入口，生成 `build/vtb`。当前阶段打开 `/dev/video0`，请求并核对 `MJPG 1280×720 @ 30fps`；还没有申请帧缓冲或开始采集。
- `include/vtb/v4l2_camera.hpp`、`src/v4l2_camera.cpp`：单个 `V4l2Camera` 类保存请求参数、驱动协商结果和文件描述符，并负责设备打开、能力检查、格式与帧率协商。
- `examples/camera-info/main.cpp`：归档的摄像头信息示例，生成 `build/camera-info`。程序固定打开 `/dev/video0`，打印摄像头支持的格式、分辨率和帧率；它不会读取画面。

在按 `AGENTS.md` 准备好的远端 ARM64 交叉编译任务目录中构建：

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

运行：

```bash
# 新的主程序
./build/vtb

# 归档的摄像头信息示例
./build/camera-info
```

阅读摄像头信息示例时，先看 `open` 如何得到设备 fd，再按“格式 → 分辨率 → 帧率”看三个查询循环，最后看 `close`。

## 公共媒体输出层

公共输出层是一组进程内 C++ 库。调用方链接所需目标，创建 `OutputManager`，再把已经编码或格式化的媒体包交给 `publish()`；库负责分发到独立的 RTSP、UVC 和 UAC2 出口。它不启动独立服务，不要求 IPC，不执行视频或音频编解码，也不创建或修改 USB Gadget configfs 配置。设计理由、失败边界和验证指标见 [`docs/stream-output-abstraction.md`](docs/stream-output-abstraction.md)。

主要入口：

- `StreamDescriptor`：注册固定的视频或音频格式；
- `MediaPacket`：携带流 ID、单调时钟纳秒时间戳和不可变共享负载；
- `OutputManager`：管理出口、绑定、有界队列、工作线程、重试和状态；
- `RtspOutput`：使用 libavformat 向 RTSP/MediaMTX 发布已编码数据；
- `UvcOutput`：使用 V4L2 Video Output 向已配置的 UVC Gadget 节点写帧；
- `Uac2Output`：使用 ALSA PLAYBACK 向已确认映射的 UAC2 Gadget PCM 写入 PCM 数据。

三个后端默认启用，CLI 示例默认关闭：

```text
VTB_ENABLE_RTSP_OUTPUT=ON
VTB_ENABLE_UVC_OUTPUT=ON
VTB_ENABLE_UAC2_OUTPUT=ON
VTB_BUILD_OUTPUT_CLI=OFF
```

同一 CMake 工程中的调用方可以直接链接聚合库：

```cmake
target_link_libraries(your_pipeline PRIVATE vtb::output)
```

只需要部分能力时，也可以分别链接 `vtb::output_core`、`vtb::output_rtsp`、`vtb::output_uvc` 或 `vtb::output_uac2`。

目标 ARM64 sysroot 需要提供 FFmpeg 的 `libavformat`、`libavcodec`、`libavutil` 开发文件及 ALSA 开发文件。实际构建遵守 `AGENTS.md` 的远端交叉编译流程；缺少工具链或 sysroot 时不回退到 Mac 或 RK3588 本地构建。

只验证公共契约、队列和 FakeOutput 测试时，可以在远端 ARM64 交叉构建中显式关闭设备后端：

```bash
cmake -S . -B build-core -G Ninja \
  -DVTB_ENABLE_RTSP_OUTPUT=OFF \
  -DVTB_ENABLE_UVC_OUTPUT=OFF \
  -DVTB_ENABLE_UAC2_OUTPUT=OFF
```

需要人工诊断生命周期时，可显式增加 `-DVTB_BUILD_OUTPUT_CLI=ON` 构建可选的 `vtb-output-cli` 示例：

```text
help
register-video <id> <version> <format> <width> <height> <fps_num> <fps_den> <encoded 0|1> [codec_config_hex]
register-audio <id> <version> <format> <rate> <channels> <encoded 0|1> [codec_config_hex]
add-rtsp <id> <url> [queue]
add-uvc <id> <device> [queue]
add-uac2 <id> <pcm> [queue]
bind <output-id> <stream-id>
start <output-id>
stop <output-id>
list
remove <output-id>
quit
```

这个 CLI 不属于库的运行依赖，也不生成媒体包，只用于人工验证配置、生命周期和状态。业务处理链在自己的进程中直接调用 `OutputManager::publish()`。执行 CLI 的 `start` 会打开网络或设备资源；UVC/UAC2 的物理测试需要先确认 Gadget、设备路径和 ALSA 方向，并按测试授权单独进行。

## RTSP 推流

本项目使用 MediaMTX `v1.21.2` Linux ARM64 官方可执行文件作为 RTSP 服务。FFmpeg 从 `/dev/video0` 读取 `MJPG 1280x720 @ 30 fps`，解码后以低延迟 H.264 Baseline 编码发布到 `/camera`。MediaMTX 二进制保留在本机 `bin/mediamtx`，Git 仓库只保存安装脚本与配置。

首次克隆后在 RK3588 上运行：

```bash
./scripts/install-mediamtx.sh
```

安装脚本固定使用官方 [MediaMTX v1.21.2 发布包](https://github.com/bluenviron/mediamtx/releases/tag/v1.21.2) 的 `linux_arm64` 版本，并在安装前校验官方发布的 SHA-256：`180a235374a548d9133cdd5e20f5dbd8b01317be1c86e0d7e66df585dc42ed6f`。本机已有 `bin/mediamtx` 时，脚本不会覆盖。

## 文件

- `bin/mediamtx`：MediaMTX Linux ARM64 可执行文件。
- `mediamtx.yml`：仅启用 RTSP/TCP `8554` 端口的配置。
- `scripts/run-mediamtx.sh`：前台运行 MediaMTX。
- `scripts/install-mediamtx.sh`：首次安装 MediaMTX 并校验发布包。
- `scripts/publish-camera.sh`：前台运行摄像头发布端。
- `scripts/start-stream.sh`：后台启动服务和摄像头推流。
- `scripts/stop-stream.sh`：停止后台进程。
- `runtime/`：运行日志与 PID 文件，由启动脚本生成。

## 启动

```bash
./scripts/start-stream.sh
```

本机拉流地址：

```text
rtsp://127.0.0.1:8554/camera
```

局域网拉流地址（把 `<RK3588_IP>` 替换为板子的实际 IP）：

```text
rtsp://<RK3588_IP>:8554/camera
```

客户端可使用：

```bash
ffplay -rtsp_transport tcp rtsp://<RK3588_IP>:8554/camera
```

## 停止

```bash
./scripts/stop-stream.sh
```

## 日志

```bash
tail -f runtime/mediamtx.log
tail -f runtime/camera.log
```
