# RK3588 USB 摄像头示例与 RTSP 推流

Mac 与 RK3588 的直连调试、上网优先级和仓库同步脚本见 [`network/README.md`](network/README.md)。

## C++ 程序

- `src/main.cpp`：新的 VTB 主程序入口，生成 `build/vtb`。
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
