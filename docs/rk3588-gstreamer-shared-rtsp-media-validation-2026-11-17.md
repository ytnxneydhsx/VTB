# RK3588 GStreamer shared RTSP media 回归排查与验证

## 1. 目标

本次回到 `GstRTSPMediaFactory` 的官方共享媒体路径，验证一个实时 H.264
`GstRTSPMedia` 能否同时服务多个 RTSP/TCP 客户端，并在最后一个客户端断开、
media 被 `unprepare` 后可靠地重新 `prepare`。

验收条件：

1. 应用每帧只向一个共享 RTSP `appsrc` 提交一次 H.264 Buffer；
2. 首个客户端连接后能够取得 IDR 并连续解码；
3. 客户端断开后再次连接，`DESCRIBE`、`SETUP` 和 `PLAY` 不返回 503；
4. 两个客户端并发解码时共用一个 MPP H.264 编码器和一个 `rtph264pay`；
5. 晚加入客户端能够从关键帧开始解码；
6. WebRTC 和 UVC 分支继续工作；
7. 正常停止后端口、摄像头和相关线程释放；
8. 真实命令、错误、修改尝试和验证边界保留在本文档中。

## 2. 修改前基线

开始时仓库状态：

- 分支：`main`；
- 提交：`b29f860c5880f2ba2c51f9b71592a02f507ef986`；
- `origin/main` 指向同一提交；
- 工作区已有与本任务无关的修改和未跟踪文件，本次不覆盖、不清理，也不纳入提交。

修改前 RTSP 路径：

```text
mpph264enc → h264parse → appsink
                            ↓ 应用逐客户端循环
            client A appsrc → queue → rtph264pay
            client B appsrc → queue → rtph264pay
```

Factory 使用 `shared=false`。每个客户端拥有独立 `GstRTSPMedia`、`appsrc`、
`queue`、`rtph264pay` 和时间戳。应用缓存最近 IDR，并对每个客户端执行一次
`gst_buffer_copy()` 和 `gst_app_src_push_buffer()`。

## 3. 历史失败与排查假设

历史 shared media 尝试记录过以下现象：第一次连接工作；客户端断开使 media
进入 `unprepare`；第二次 `DESCRIBE` 返回 503；live `appsrc` 的 preroll 与应用层
prepared 门控互相等待。

本次先验证 `shared=true + reusable=true`。官方文档说明：

- [`shared`](https://gstreamer.freedesktop.org/documentation/gst-rtsp-server/rtsp-media.html#gst_rtsp_media_set_shared)
  让多个客户端共享同一个 media pipeline；
- [`reusable`](https://gstreamer.freedesktop.org/documentation/gst-rtsp-server/rtsp-media.html#gst_rtsp_media_set_reusable)
  允许同一个 media 在 `unprepare` 后再次 prepare；
- 真正的 RTP/RTCP 分发由 `GstRTSPStream` 完成。

为避免 preroll 等待，本次同时保留以下策略：

- `appsrc need-data` 在 pipeline 需要数据时注入缓存的最近 IDR；
- 每个 media 生命周期使用独立、从 0 开始的连续时间戳；
- 新 media 开始前丢弃 delta frame，首个成功入队的 Buffer 必须是 IDR；
- `block=false` 和有界 leaky appsrc/queue 保持实时流非阻塞；
- `GST_RTSP_LOWER_TRANS_TCP` 固定为 RTP/RTCP over RTSP/TCP interleaved。

## 4. 目标结构

```text
主媒体 pipeline
NV12 → mpph264enc → h264parse → appsink
                                      ↓ 每帧只提交一次
当前活动的共享 RTSP media
appsrc → queue → rtph264pay → GstRTSPStream
                                  ├─ transport A → client A
                                  └─ transport B → client B
```

WebRTC 继续从 `h264parse` 后的编码 tee 取得同一个 H.264 流；UVC 继续使用 raw
NV12 tee 的 MJPEG 分支。本次不移动编码器，也不改变 RGA NV12 Lease 和 DMA-BUF
生命周期。

## 5. 实施记录

### 5.1 远端环境与源码传输

按项目规则，构建前尝试只读检查固定服务器 `fx@10.191.32.121`：

```text
Connection timed out during banner exchange
Connection to UNKNOWN port 65535 timed out
```

随后重试成功。远端环境为 x86_64、80 个逻辑 CPU，构建使用项目固定的
`aarch64-godot-linux-gnu-g++ 13.2.0` 和 Buildroot/Godot SDK sysroot。

第一次 `rsync` 误把本地 13 GB 运行目录包含进传输，发现后在约 402 MB 时终止，并只
删除本任务未完成的远端 `source` 临时目录。正式源码使用 `git archive HEAD` 加本任务
文件组成 `source-clean`，没有覆盖其他远端任务目录。

### 5.2 源码修改

第一版删除了每客户端 `RtspClientStream` 和应用层客户端循环，改为：

- factory `shared=true`；
- media `reusable=true`；
- 保存唯一 shared media/source 引用；
- `need-data` 注入缓存 IDR；
- H.264 appsink 回调每帧只 push 一次；
- `unprepared` 复位时间戳与关键帧门控；
- 清理时解除 callbacks、unprepare 并释放 shared media/source。

### 5.3 第一版板端失败：同一个 reusable media 重连后停流

第一版 ARM64 构建和板端启动成功。第一次 `ffprobe` 能得到：

```text
codec_name=h264
width=1920
height=1080
r_frame_rate=60/1
```

`ffprobe` 断开后，日志出现：

```text
VTB_RTSP_MEDIA_CONFIGURED shared=1 reusable=1
VTB_RTSP_MEDIA_UNPREPARED reusable=1
```

紧接着的 `ffmpeg` 连接完成了 RTSP 握手，却在 37 秒内没有解码出任何帧。打开
`rtspmedia`、`rtspclient`、`rtspmediafactory` 和 `appsrc` 调试日志后确认：

1. factory 命中缓存的同一个 media；
2. media 再次完成 `UNPREPARED → PREPARING → PREPARED → PLAYING`；
3. `rtsp-h264-source` 只消费缓存 IDR 和下一帧两个 Buffer；
4. 随后 appsrc 队列保持 8 个 Buffer 满载，持续覆盖旧帧，客户端没有收到可解码流；
5. 连接关闭后 appsrc 进入 flushing。

因此，这次问题已经缩小到“复用已经 unprepare 的同一条 appsrc/payloader pipeline”。
它没有复现历史的 503，但媒体数据面仍然失效。

### 5.4 最终生命周期

官方 media factory 源码会缓存 shared media；对非 reusable media，它在
`unprepared` 信号后把旧实例从缓存移除。最终实现采用：

```text
同时在线：client A、B、C → 同一个 shared GstRTSPMedia
最后一个客户端离开：旧 media unprepare → factory 移出缓存 → 应用释放引用
后续首个客户端连接：factory 创建新的 shared GstRTSPMedia
```

对应代码修改为：

- factory 保持 `shared=true`；
- media 使用默认 `reusable=false`；
- `unprepared` 回调清空唯一 appsrc/media 引用、解除 appsrc callbacks，并复位时间戳与
  IDR 门控；
- 后续 `media-configure` 保存新的唯一 appsrc/media；
- 同一活动生命周期内，应用仍然每帧只 push 一次，所有客户端由同一个
  `GstRTSPStream` 分发；
- 最后一个客户端离开后的重建由官方 factory 生命周期完成。

## 6. 构建与验证记录

### 6.1 远端 ARM64 构建

初次链接使用了旧 LibGodot，真实错误为缺少：

```text
openvt_libgodot_application_initialize_v7
openvt_libgodot_application_wait_gpu_render
```

随后换成已经包含 ABI v7 符号的既有 LibGodot：

```text
/hdd/fx-builds/vtb-direct-rgba-dmabuf-20261117-01/godot-source/bin/
libgodot.linuxbsd.template_release.arm64.so
SHA-256: 9bde957ae77c5da95987a61943c17f00b78d837550e7a0776bf66e2b3ec78a41
```

最终 `vtb-live2d` 与全目标构建通过。最后一次增量构建也通过。复制产物时曾使用错误的
`build/apps/vtb-live2d/vtb-live2d` 路径；CMake 实际输出位于 `build/vtb-live2d`，修正后
重新生成校验。

```text
远端任务目录：/hdd/.fx/vtb-builds/shared-rtsp-20261117-G1RpiI
产物：ELF64 / AArch64 / interpreter /lib/ld-linux-aarch64.so.1
最终产物 SHA-256：b4c46de624bf10b2073a79cc36c7ea2eda63e41bd00eec1a72f077b09e71f60e
```

### 6.2 板端部署与依赖

独立部署目录：

```text
/home/huangnv/VTB/runtime/shared-rtsp-20261117-01
```

板端缺少 `file` 命令，第一次核验在该命令处停止。随后使用 `readelf` 确认 ELF64、
AArch64 和动态解释器，并用 `ldd` 确认 LibGodot、OpenCV 5、RGA、RKNN、Mali、
GStreamer 和 RTSP Server 依赖全部解析。部署后的二进制 SHA-256 与远端产物一致。

### 6.3 RTSP/TCP 功能回归

客户端统一使用：

```bash
ffmpeg -rtsp_transport tcp \
  -i rtsp://192.168.77.2:8554/live2d \
  -map 0:v:0 -frames:v N -f null -
```

结果：

| 场景 | 结果 |
|---|---|
| 第一个客户端 | 1920×1080、H.264 High、60 fps，120/120 帧，退出码 0 |
| 完全断开后第二次连接 | 120/120 帧，约 2 秒完成，退出码 0 |
| 客户端 A 持续观看 | 360/360 帧，退出码 0 |
| 客户端 B 在 A 启动 2 秒后加入 | 240/240 帧，退出码 0 |
| 连续 10 次连接、解码 30 帧、断开 | 10/10 成功，失败数 0 |

表 1：RTSP/TCP 板端共享 media 回归结果。

客户端 B 的首个媒体时间位于共享流的当前进度，且并发期间只创建一个 media，说明 A、B
共享同一个活动 pipeline。完整测试一共产生 13 个活动生命周期，服务端最终日志为：

```text
configured=13
released=13
```

没有发现 503、prepare/preroll 失败、H.264 push 失败或 media session failure。

### 6.4 WebRTC、UVC 与资源清理

- WebRTC 静态页面 `http://192.168.77.2:8080/` 返回 HTTP 200，大小 17140 字节；
- 8443 端口监听，日志显示 `registered as [Producer]`；该端口是 WebSocket 信令端口，
  普通 HTTP/HTTPS `curl` 不构成有效 WebSocket 会话；
- `/dev/video2` 在运行期间由进程持有两个 fd，MJPEG 计数持续增长；
- 最终结果：`submitted=7584`、`h264=7584`、`mjpeg=3803`、
  `rtsp_pushed=1179`、`rtsp_wait_keyframe_dropped=1`、`raw_dropped=6`；
- SIGINT 优雅退出成功，指标 reporter 均为 `failed=0`；
- 退出后 8554、8080、8443、11575 全部释放，`/dev/video0` 和 `/dev/video2` 无占用。

验证边界：本次实际解码验证覆盖 RTSP/TCP；factory 配置未开放 UDP。WebRTC 完成
HTTP/信令 producer 健康检查，没有重新进行浏览器端媒体播放。UVC 完成板端设备打开与
MJPEG 编码计数检查，没有重新进行 Mac 端 UVC 采集。

## 7. 结论

官方 shared media API 在 RK3588 当前 GStreamer 1.28.9 环境可用。稳定组合为
`shared=true + reusable=false`：同时在线客户端共享一个 appsrc、queue、
`rtph264pay` 和 `GstRTSPStream`；最后一个客户端离开后回收旧 media；后续连接创建新的
共享 media。应用每个 H.264 Buffer 只 push 一次，底层 transport 由 GStreamer 分发。

`reusable=true` 的同实例重连路径在本次板端验证中会停流，调试证据已保留在部署目录的
`vtb-live2d-reusable-debug.log`。最终实现通过断开重连、双客户端并发、中途加入、连续
10 次重连和资源释放验证，可以保留并提交。
