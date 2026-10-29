# RK3588 USB 摄像头示例与 RTSP 推流

Mac 与 RK3588 的直连调试、上网优先级和仓库同步脚本见 [`network/README.md`](network/README.md)。

## C++ 程序

- `src/main.cpp`：新的 VTB 主程序入口，生成 `build/vtb`。当前阶段打开 `/dev/video0`，请求并核对 `MJPG 1280×720 @ 30fps`；还没有申请帧缓冲或开始采集。
- `include/vtb/capture_config.hpp`：请求参数和驱动协商结果。
- `include/vtb/v4l2_camera.hpp`、`src/v4l2_camera.cpp`：V4L2 设备打开、能力检查、格式与帧率协商。
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

## 本地人脸关键点模型

人脸跟踪模块采用与 VTube Studio 摄像头跟踪同源的 OpenSeeFace 模型。当前固定使用：

- `mnv3_detection_opt.onnx`：人脸检测。
- `lm_model3_opt.onnx`：高质量 66 点人脸关键点检测。

下载到本地并校验模型：

```bash
./scripts/install-openseeface-models.sh
```

模型保存在 `models/openseeface/`，具体来源、版本和转换说明见 [`models/openseeface/README.md`](models/openseeface/README.md)。下述本地预览只绘制跟踪结果；浏览器推流链路见后续章节。

安装 OpenSeeFace Python 运行代码并启动黑底三维点预览：

```bash
./scripts/install-openseeface-runtime.sh
python3 -m venv build/openseeface-venv
build/openseeface-venv/bin/python -m pip install -r requirements/openseeface-preview.txt
build/openseeface-venv/bin/python tools/openseeface_3d_preview.py --capture 0
```

预览只绘制整体置信度和 66 个三维点，不显示摄像头原始画面。需要显示官方人脸拓扑连线时增加 `--show-lines`。按 `Q` 或 `Esc` 关闭窗口。

## 浏览器低延迟预览

RK3588 上的 EMEET SmartCam C60E 4K 使用 `/dev/video0` 输出图像，支持 MJPEG `1280×720 @ 60fps`；`/dev/video1` 是 UVC 元数据节点。浏览器链路为：

```text
/dev/video0 → OpenSeeFace/RKNN NPU → 黑底66点画布 → MPP H.264 → MediaMTX → WebRTC浏览器
```

浏览器推流默认使用 `rknn` 后端：`mnv3_detection_fp16.rknn` 在 NPU Core0
执行人脸检测，`lm_model3_fp16.rknn` 在 NPU Core1 执行 66 点推理。只有诊断
CPU 版本时才设置 `VTB_FACE3D_BACKEND=onnx`。

前台启动：

```bash
./scripts/run-face3d-browser-stream.sh
```

需要由同一个 OpenSeeFace 进程同时输出人脸检测画布和 Live2D
参数时，指定 Godot 的本机 UDP 端口：

```bash
VTB_FACE3D_LIVE2D_UDP=127.0.0.1:11574 \
./scripts/run-face3d-browser-stream.sh
```

该模式只打开一次摄像头，同时发布 `/face3d` 检测流并向
Live2D 渲染端发送参数；Live2D 画面使用独立的 `/live2d` 流。
在 Mac 上启动左侧人脸检测、右侧 Live2D 的双流监看页：

```bash
./scripts/run-dual-stream-monitor.sh
```

默认地址为 `http://192.168.77.1:8900/dual-stream-monitor.html`。Mac 直连
网卡地址或端口变化时，分别设置 `VTB_MONITOR_BIND_ADDRESS` 和
`VTB_MONITOR_PORT`。

监看页的“以当前正脸校准”按钮会请求 RK 上的 OpenSeeFace 进程清空
原中立姿态，再用接下来 30 个有效正脸帧计算新零点。校准期间页面
显示进度，Live2D 头部参数先回到中立值。控制口默认只绑定
`192.168.77.2:11575`，浏览器请求来源限制为
`http://192.168.77.1:8900`。

右侧 Live2D 区域支持滚轮缩放、拖动平移和双击恢复全身，也可以选择全身、
半身或面部预设。操作通过控制口更新 Godot 渲染前的模型变换，输出仍由
1920×1080 SubViewport 从 Live2D 原始纹理重新渲染，不对浏览器视频做放大
后处理。

Mac 浏览器访问：

```text
http://rk3588-ubuntu.local:8889/face3d
```

MediaMTX 使用 TCP `8889` 提供 WebRTC 页面和信令，局域网媒体使用 UDP
`8189`。Mac 与 RK 无法直接互访时，通过现有 FRP SSH 建立 TCP 媒体回退：

```bash
ssh -fN \
  -L 127.0.0.1:8889:127.0.0.1:8889 \
  -L 127.0.0.1:18189:127.0.0.1:18189 \
  rk3588-aliyun
```

随后访问 `http://127.0.0.1:8889/face3d/`。当前链路通过
`vtb-mpp-output-publisher` 使用 RK3588 MPP 硬件编码 H.264。采集线程始终覆盖
旧帧，推理只读取最新帧，避免处理速度下降时持续累积延迟。

验证时分别记录摄像头取帧到推理完成、画布生成到 H.264 输出、WebRTC 接收播放的 P50/P95 延迟。首版的主要目标是保持延迟有界；帧率随实际模型吞吐量变化。

## OpenSeeFace 驱动 Live2D

主分支提供 OpenSeeFace/RKNN 到 Godot/Ayagami 的参数桥接：发送端进行中立头姿
校准、标准参数映射和按时间平滑，Godot 端只监听本机 UDP，读空积压数据报并
应用最新快照。未启用参数端口时，原有 Live2D 固定动画基线保持不变。

协议、映射表、启动方式、失效恢复和当前尚未连接的实时推流边界见
[`docs/live2d-face-tracking-bridge.md`](docs/live2d-face-tracking-bridge.md)。

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
