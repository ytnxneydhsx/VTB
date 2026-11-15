# ADR 0005：用 GStreamer 管理 Live2D 的网络与 UVC 双输出

## 状态

已实现。当前宿主把带槽位租约的 NV12 DMA-BUF 移交给 GStreamer；GStreamer 负责
分支、线程、背压和协议输出，两个分支分别使用独立的 Rockchip MPP 编码会话。

## 背景

Godot/LibGodot 完成 Live2D 渲染、Viewport 纹理取得、GPU blit 与 GPU 完成等待，随后
把 RGBA DMA-BUF 同步借给宿主。宿主 RGA 只执行一次 RGBA 到 NV12 转换。网络输出需要
H.264，UVC Gadget 当前协商 MJPEG，因此两个输出不能直接共享同一份编码数据。

原有实现让 Godot 所有者线程同步执行两个 MPP 编码，再把编码包交给两个自建输出队列。
这种结构会让渲染线程承担编码等待，也重复实现了 GStreamer 已具备的分支、线程和背压
机制。

## 决策

```text
Godot / GPU
    ↓ RGBA DMA-BUF
RgaNv12FrameTransformer（四槽 NV12 DMA-BUF 池）
    ↓ move Nv12FrameLease
GStreamer appsrc（GstBuffer 持有 DMA-BUF 与 Lease）
    ↓
   tee
    ├─ queue（容量 2，满时丢旧帧）→ appsink → MPP H.264
    │                                      ↓
    │                              H.264 appsrc → h264parse
    │                                      ↓
    │                         rtspclientsink/TCP → MediaMTX
    │
    └─ queue（容量 2，满时丢旧帧）→ appsink → MPP MJPEG
                                           ↓
                                   MJPEG appsrc → jpegparse
                                           ↓
                                   uvcsink → /dev/video2
```

- `Nv12FrameSink::submit()` 接收可移动的 `Nv12FrameLease`。提交成功后，媒体会话取得
  所有权。
- `GstDmaBufAllocator` 用复制后的 DMA-BUF fd 创建 `GstMemory`。Lease 绑定到同一个
  `GstBuffer` 的 qdata；tee 两侧共享其引用计数，最后一个引用释放时 Lease 析构并把
  RGA 槽位归还目标池。
- 原始 NV12 不进入项目自建帧队列。`appsrc`、`tee`、两条 `queue` 和 `appsink` 提供
  异步传递、线程调度与有界背压。
- 每个分支的 appsink 回调独占一个 `MppEncoderSession`。MPP 输入导入按
  `buffer_id + stream_generation` 缓存；两个编码上下文互不共享码控、GOP 或包状态。
- MPP 编码包复制到 GStreamer 自有的编码 `GstBuffer` 后，原 MPP packet 可以立即释放。
- H.264 下游的 ForceKeyUnit 事件转换为 `request_idr()`，由 H.264 分支线程实际调用
  `mpp_control()`。网络队列溢出时也会请求新的 IDR。
- RTSP 发布固定使用 AVP/TCP，避免 UDP 传输协商失败后进入不兼容的 profile 重试。
- UVC 低于渲染帧率时按帧序号做整数时间表降采样。例如 60 fps 渲染和 30 fps UVC
  会每两帧编码一张 MJPEG。
- RGA 与 MPP 的硬件核心由 Rockchip 驱动调度；应用不做 CPU 或硬件核心亲和性绑定。

## 并发与背压

Godot 所有者线程只完成渲染、GPU 同步、RGA 转换和一次非阻塞 appsrc 提交。H.264 与
MJPEG 的编码回调分别由 GStreamer 分支流线程调用。两条 raw queue 都是容量 2 的
downstream-leaky 队列，慢分支丢弃自己的旧帧，不等待另一分支。

RGA NV12 池包含四个槽位，用于覆盖 appsrc 与两个分支中的少量在途帧。GStreamer 最终
释放 Lease 是槽位可复用的唯一通知，因此无需另写“消费完成”消息或归还队列。

## 代价与边界

- 编码包进入协议 pipeline 时有一次内存复制；原始 NV12 像素保持 DMA-BUF 传递。
- MPP 通过 appsink 回调接入，当前没有实现通用的 GStreamer MPP encoder plugin。这个
  边界让现有 `MppEncoderSession` 可以复用，同时让 GStreamer 拥有分支和背压。
- 动态改变分辨率、stride、帧率或 stream generation 需要显式重建 pipeline；运行中
  未协商的契约变化会被拒绝。
- UVC Gadget 描述符、UDC 绑定和 USB 主机枚举属于部署阶段。板端持续产生 MJPEG 并不
  等价于 Mac 或树莓派已经成功打开摄像头。
- 当前 ACM+UVC 组合保留 UVC endpoint。NCM 会与 UVC streaming endpoint 竞争，未在
  本决策中重新加入。

## 本轮验证（2026-11-16）

1. 使用固定 ARM64 Buildroot/Godot SDK、Rockchip sysroot 与定制 GStreamer 1.28.9
   sysroot 完成交叉编译；产物经 `file` 确认为 AArch64 ELF。
2. 定制运行时包含 appsrc、appsink、tee、queue、h264parse、rtspclientsink、jpegparse
   和 uvcsink 所需插件。
3. RK3588 上双分支持续运行超过 7500 个渲染帧；H.264 与 MJPEG 指标持续增长，未出现
   GStreamer pipeline error。
4. Mac 端 `ffprobe` 识别 `rtsp://192.168.77.2:8554/live2d` 为 H.264、1920×1080、
   60 fps；双流监看页面中的 Live2D video 达到 `readyState=4` 并持续播放。
5. 板端 `/sys/class/udc/fc000000.usb/state` 为 `configured`，`/dev/video2` 对应
   `dwc3-gadget` 的 `uvc.usb0`；MJPEG 分支持续编码和提交。
6. 本轮 USB 主机连接在树莓派，公网 SSH 隧道当前会断开，Mac 也未直接接入 OTG，
   因此尚未在 USB 主机侧重新完成实际抓帧。该项保持为待验收，不沿用旧版本抓帧结果。
