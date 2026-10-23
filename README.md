# RK3588 USB 摄像头采集与 RTSP 推流

## C++ V4L2 环形缓存

`vtb-capture` 使用 V4L2 MMAP 从 USB 摄像头取帧，随后立即复制到固定容量的线程安全环形缓存中。缓存写满后持续覆盖最旧槽位，不保存帧到磁盘。当前连接的 Realtek USB Camera 使用 `/dev/video0` 作为图像节点，默认采集其原生支持的 `MJPG 1280x720 @ 30 fps`。

构建：

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

持续采集，按 `Ctrl+C` 结束：

```bash
./build/vtb-capture
```

短时验证 60 帧：

```bash
./build/vtb-capture --frames 60
```

使用未压缩的 `YUYV 640x480 @ 30 fps`：

```bash
./build/vtb-capture \
  --format YUYV \
  --width 640 \
  --height 480 \
  --fps 30
```

常用选项可通过以下命令查看：

```bash
./build/vtb-capture --help
```

环形缓存默认包含 8 个槽位，每个槽位保存一帧数据及应用序号、V4L2 驱动序号、采集时间戳、像素格式和尺寸。`FrameRingBuffer::latest()` 可获取最新帧快照，`waitForLatest()` 可等待比指定序号更新的帧。快照拥有独立内存，因此缓存槽位被循环覆盖时，消费线程仍可安全处理已有帧。

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
