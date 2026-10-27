# RK3588 USB 摄像头示例与 RTSP 推流

Mac 与 RK3588 的直连调试、上网优先级和仓库同步脚本见 [`network/README.md`](network/README.md)。

## C++ 程序

- `src/main.cpp`：新的 VTB 主程序入口，生成 `build/vtb`。当前阶段打开 `/dev/video0`，请求并核对 `MJPG 1280×720 @ 30fps`；还没有申请帧缓冲或开始采集。
- `include/vtb/capture_config.hpp`：请求参数和驱动协商结果。
- `include/vtb/v4l2_camera.hpp`、`src/v4l2_camera.cpp`：V4L2 设备打开、能力检查、格式与帧率协商。
- `examples/camera-info/main.cpp`：归档的摄像头信息示例，生成 `build/camera-info`。程序固定打开 `/dev/video0`，打印摄像头支持的格式、分辨率和帧率；它不会读取画面。

构建：

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

## 本地人脸关键点模型

人脸跟踪模块采用与 VTube Studio 摄像头跟踪同源的 OpenSeeFace 模型。当前固定使用：

- `mnv3_detection_opt.onnx`：人脸检测。
- `lm_model3_opt.onnx`：高质量 66 点人脸关键点检测。

下载到本地并校验模型：

```bash
./scripts/install-openseeface-models.sh
```

模型保存在 `models/openseeface/`，具体来源、版本和阶段边界见 [`models/openseeface/README.md`](models/openseeface/README.md)。当前阶段不发送跟踪数据，也不输出摄像头原始画面。

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
/dev/video0 → OpenSeeFace → 黑底66点画布 → H.264 → MediaMTX → WebRTC浏览器
```

前台启动：

```bash
./scripts/run-face3d-browser-stream.sh
```

Mac 浏览器访问：

```text
http://rk3588-ubuntu.local:8889/face3d
```

MediaMTX 使用 TCP `8889` 提供 WebRTC 页面和信令，使用 UDP `8189` 传输媒体。当前 FFmpeg 使用 `libx264` 的无 B 帧、`zerolatency` 配置；RK 上现有 FFmpeg 没有 `h264_rkmpp`，MPP 设备权限也尚未提供给普通用户，因此硬件编码留待单独验证。采集线程始终覆盖旧帧，推理只读取最新帧，避免处理速度下降时持续累积延迟。

验证时分别记录摄像头取帧到推理完成、画布生成到 H.264 输出、WebRTC 接收播放的 P50/P95 延迟。首版的主要目标是保持延迟有界；帧率随实际模型吞吐量变化。

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
