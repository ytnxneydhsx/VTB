# ADR 0006：用 Rockchip GStreamer MPP 编码元件替代应用层编码会话

## 状态

已实现并通过远程 ARM64 构建、RK3588 双编码分支和 RTSP 新客户端接入验证。
UVC 分支已在板端持续编码，Mac USB 主机尚未枚举出该摄像头，因此主机抓帧仍待验证。

## 背景

ADR 0005 建立了 raw appsrc、tee、两条有界 queue 和 RTSP/UVC 输出，但两个分支仍通过
appsink 回到应用层，由 `MppEncoderSession` 手工导入 DMA-BUF、调用 Rockchip MPP、复制
编码包，再经第二组 appsrc 返回 GStreamer。这个边界可以工作，也保留了既有 MPP 包装，
代价是应用需要维护 MPP context、输入缓存、GOP/码控、IDR 请求、时间戳和关闭顺序。

Rockchip 的 GStreamer 插件已经提供 `mpph264enc` 和 `mppjpegenc`。两者接受
`video/x-raw(memory:DMABuf)`，能导入匹配 stride 的 NV12 DMA-BUF；H.264 元件还通过
GStreamer VideoEncoder 的关键帧机制处理下游 ForceKeyUnit 请求。因此应用层编码会话
不再承担独有功能。

## 决策

```text
RgaNv12FrameTransformer（NV12 DMA-BUF 池）
    ↓ move Nv12FrameLease
raw appsrc（DMA-BUF GstMemory 持有 Lease）
    ↓
   tee
    ├─ leaky queue → mpph264enc → h264parse
    │                               ↓
    │                  appsink → 进程内 GstRTSPServer
    │
    └─ leaky queue → videorate → raw caps → mppjpegenc → jpegparse
                                                        ↓
                                                uvcsink → UVC Gadget
```

- 删除应用内的 `MppEncoderSession` 和两个 appsink/encoded-appsrc 桥接。
- `GStreamerMediaSession` 创建一条组合 pipeline；两个编码元件各自管理 MPP context。
- 输入 caps 固定为 `video/x-raw(memory:DMABuf), format=NV12`。Lease 绑定到
  `GstMemory`，确保 videorate 产生浅拷贝或编码器保留输入内存引用时，RGA 槽位仍不会
  提前归还。
- H.264 使用 high profile、固定 QP 26、两秒 GOP、每个 IDR 携带 header。GOP 配置只
  约束 I 帧间隔；媒体会话每两秒发送一次标准 downstream ForceKeyUnit 事件，插件把
  事件转换成 MPP IDR 请求。下游发起的 upstream ForceKeyUnit 请求也直接到达
  `mpph264enc`。
- UVC 分支由 `videorate drop-only=true` 和 capsfilter 把渲染帧率降到 UVC 帧率，MJPEG
  质量保持 80。
- 两个编码器的 `max-pending` 都限制为 2；分支 queue 保持容量 2、满时丢旧帧。
- 输入布局匹配时，两个编码器各自导入同一块 NV12 DMA-BUF，并保留原始 GstMemory 的
  引用。编码后的 H.264/JPEG 包使用插件内部的稳定拷贝 buffer，供异步 parser 和协议
  sink 使用。
- 插件源码固定在 JeffyCN `gstreamer-rockchip` 提交
  `bff0659a51c13585b3c9c9a68aa8c435beeb3fe1`（tree
  `faea26be29a4c2ee68d224625723832598488e22`），并随项目的 GStreamer 1.28.9 运行时构建。
  项目补丁增加显式 MPP sysroot、逐输入帧 IDR metadata、正确的 MppPacket 有效数据
  偏移处理以及 RGA 回退路径的句柄管理。补丁均随固定源码版本构建并单独记录。

## 为什么采用这个边界

保留应用层 MPP 包装是改动更小的方案，也方便逐项控制 MPP API。当前需求只需要插件已
覆盖的 H.264/MJPEG 属性、DMA-BUF 导入和关键帧控制；继续保留包装会重复 GStreamer
编码元件已经维护的状态。新边界让 appsrc 到协议 sink 的 buffer、事件、EOS 和错误都留在
一条 GStreamer 图中，并删除一次应用层编码 packet 拷贝。

## 代价与边界

- 运行时新增固定版本的 `libgstrockchipmpp.so`，并间接依赖板端匹配的
  `librockchip_mpp.so`。插件、MPP 运行库和内核驱动必须按当前 BSP 验证。
- RGA 转换回退仍适用于输入布局与编码器要求不匹配的情况。直导验收需要分别确认两个
  编码器进入导入路径。
- 应用对 MPP 的细粒度观测减少；当前通过编码器 src pad 统计输出帧，通过 queue overrun
  统计 raw 丢帧，并统计周期性及下游触发的 IDR 请求。
- 动态改变分辨率、stride、帧率或 stream generation 仍要求重建媒体会话。
- UVC Gadget 枚举和 USB 主机消费仍属于独立验收层级。

## 验收指标

1. 远端 ARM64 构建包含 `libgstrockchipmpp.so`，`gst-inspect` 可识别 `mpph264enc` 与
   `mppjpegenc`；`vtb-live2d` 不再直接 `DT_NEEDED` `librockchip_mpp.so`。
2. RK3588 上 1920×1080、60 fps RTSP 与 30 fps UVC 双分支持续运行；H.264/MJPEG 计数
   持续增长，pipeline 无错误。
3. MPP debug 日志确认输入使用 imported buffer；CPU、端到端延迟和 raw queue 丢帧与
   ADR 0005 的已验证版本对比。
4. 进程内 GstRTSPServer 客户端确认 RTSP 画面连续且重连能恢复 IDR；USB 主机确认 UVC MJPEG 能反复
   打开、抓帧和关闭。

## 当前验证

2026-11-16 完成以下验证：

1. 使用固定 Buildroot/Godot GCC 13.2.0 和 Rockchip sysroot 完整交叉编译
   GStreamer 1.28.9、`videorate`、`libgstrockchipmpp.so` 与 `vtb-live2d`；产物均为
   AArch64 ELF。
2. `libgstrockchipmpp.so` 的工厂表包含 `mpph264enc` 和 `mppjpegenc`，并直接
   `DT_NEEDED` `librockchip_mpp.so.1`。
3. `vtb_gstreamer_media_session` 和完整 `vtb-live2d` 编译、链接成功；
   `vtb-live2d` 的 `DT_NEEDED` 已不包含 `librockchip_mpp.so`。所有 CMake 目标也已
   完成交叉编译，其中的既有测试警告与本变更无关。
4. 在 RK3588 安装到独立的 `/opt/vtb/gstreamer-1.28.9` 前缀，两个编码元件均成功加载；
   MPP debug 日志确认 H.264 与 MJPEG 输入持续走 `using imported buffer`，未观察到软件
   转换输入。
5. 10 秒稳定区间内渲染增加 599 帧；进程退出汇总为 `submitted=7628`、
   `h264=7567`、`mjpeg=3823`、`raw_dropped=4`、`idr_requests=63`，pipeline error 为 0。
6. 修复插件读取可复用 MPP packet buffer 基址的问题后，8 秒 RTP 码流包含 401 个
   non-IDR slice、3 个 IDR slice 和 404 个 AUD。Mac 上新建的 RTSP/TCP 客户端在 5 秒内
   解码 248 帧 1920×1080@60 H.264 并正常退出；另一轮抓取的 301 个 RTP packet（忽略首个
   无时间戳配置包）PTS/DTS 严格递增。
7. UVC 分支在板端按约 30 fps 持续产出 MJPEG，UDC 状态为 `configured`；Mac 的
   AVFoundation 和 IOUSB 设备树未出现 `RK3588 Live2D Camera`，因此 USB 主机枚举、打开、
   抓帧和重开尚未通过。本轮不执行曾导致内核告警与 SSH 中断风险的 UDC 解绑/重绑。
8. Doxygen + Graphviz 说明书重新生成，相关页已包含新编码元件，不再包含
   `MppEncoderSession`。

远程构建记录位于
`/hdd/.fx/vtb-builds/gstreamer-mpp-20261116-5482Ng/artifacts/manifest.txt`。
CPU、端到端延迟以及 USB 主机消费仍需单独完成对比或验收。

2026-11-17 的进程内 RTSP 设计与 UVC 启动时 DMA-BUF fd 被覆盖的根因记录见 ADR 0007。
