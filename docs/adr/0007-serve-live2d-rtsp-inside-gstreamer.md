# ADR 0007：在 Live2D 进程内使用 GStreamer RTSP Server

## 状态

已实现，并通过 ARM64 交叉构建、RK3588 运行、首次连接、重连、双客户端和 H.264 解码验证。

## 背景

原链路由 `rtspclientsink` 把 H.264 推给独立 MediaMTX 进程，再由 MediaMTX 对 RTSP 客户端
提供服务。Live2D 已经使用 GStreamer 管理 raw appsrc、MPP 编码和 UVC，继续保留中间 RTSP
发布进程会增加一个进程、一条本机 RTSP 会话和一次协议层排队。当前产品只需要板端 RTSP，
不需要 MediaMTX 的 WebRTC、HLS、录制、鉴权和多协议转发能力。

## 决策

```text
NV12 DMA-BUF
  → appsrc → tee
      ├─ queue → mpph264enc → h264parse → appsink
      │                                      ↓ 浅拷贝编码 buffer
      │                         进程内 GstRTSPServer
      │                         每客户端 appsrc → queue → rtph264pay
      └─ queue → videorate → mppjpegenc → jpegparse → uvcsink
```

- `GStreamerMediaSession` 在同一进程中创建 `GstRTSPServer`，监听
  `output.rtsp_url` 的地址、端口和挂载点。
- H.264 只编码一次。appsink 把编码 buffer 分发给每个客户端自己的轻量 RTP pipeline，
  客户端数量不会增加 MPP 编码器数量。
- 每个 RTSP 客户端使用独立 appsrc、queue 和 `rtph264pay`，慢客户端通过有界 leaky queue
  隔离，不阻塞编码主分支。
- 媒体会话缓存最近一个 IDR。新客户端先取得该 IDR，再接收后续帧；客户端时间戳从零连续
  生成，避免把运行很久后的绝对 PTS 直接带入新会话。
- 新客户端到来时向 H.264 编码器请求 IDR。尚无可用关键帧时暂不启动 RTSP 服务。
- Live2D 路径不再启动 MediaMTX，也不监听其 WebRTC 端口 8889。Face3D 的历史服务和脚本
  不在本决策范围内。

## 资源与性能边界

这一边界减少了 MediaMTX 进程、本机 RTSP 发布连接和一层协议排队；GStreamer RTSP Server
与编码 pipeline 共享现有 GLib/GStreamer 运行时。每个客户端仍会增加 RTP payloader、socket
和小型队列。

板端启动跟踪确认，`uvcsink` 原先以 `device_fd + 1` 为固定目标执行 `dup3()`，覆盖了
RGA 已打开的 NV12 DMA-BUF fd。补丁 0006 使用 `F_DUPFD_CLOEXEC` 分配空闲描述符。
布局匹配的 NV12 由两个 MPP 编码器分别导入并共享原始像素，保留 GstMemory 引用直到消费
结束。临时强制逐分支拷贝的补丁 2004 已移除。

浏览器通常不能直接播放 RTSP。移除 MediaMTX 后，Mac 端应使用 ffplay、VLC、GStreamer
等 RTSP 客户端；现有依赖 MediaMTX WebRTC 端口的双流浏览器页面不能直接消费本服务。

## 当前验证

2026-11-17 完成：

1. `vtb-live2d` 与 `libgstrockchipmpp.so` 使用固定 Buildroot/Godot GCC 13.2.0 交叉编译为
   AArch64；插件从固定提交和项目补丁集重新构建成功。
2. RK3588 上监听 `192.168.77.2:8554/live2d` 和控制端口 11575；MediaMTX 与 8889 未运行。
3. 首次连接和断开重连均可解码；5 秒 ffprobe 读取 301 个 1920×1080@60 H.264 packet；
   两个并发 ffmpeg 客户端各自完成 5 秒解码且无解码告警。
4. 运行期间 fd 数量在 76～81 间随队列波动，没有持续增长；该验证区间内内核没有新的
   DMA-BUF、RGA 或 MPP 错误。
5. 正常退出汇总为 `submitted=3921`、`h264=3921`、`mjpeg=1962`、
   `rtsp_pushed=1251`、`raw_dropped=0`，端口在退出后释放。

CPU、RGA 带宽、端到端延迟以及多客户端上限仍需使用同一输入和同一输出配置做正式对比。
