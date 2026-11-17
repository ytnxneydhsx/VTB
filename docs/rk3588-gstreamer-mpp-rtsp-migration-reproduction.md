# RK3588 VTB：GStreamer MPP 与进程内 RTSP Server 改造复现记录

本文记录 2026 年 11 月 16 日至 17 日的实际调整过程，包括原始问题、架构变化、构建与部署、中间失败、插件补丁、板端验证结果和仍未完成的边界。

11 月 17 日随后通过板端启动跟踪确认并修复了 UVC 覆盖 NV12 fd 的问题。当前直导结果见第 9.6 节。第 13–14 节保留修复前的阶段性验证和产物。

## 1. 最终状态摘要

当前实现链路为：

```text
Godot RGBA
  → RGA：RGBA 转 NV12，输出 DMA-BUF
  → GStreamer appsrc
  → tee
      ├─ queue
      │   → mpph264enc
      │   → h264parse
      │   → appsink
      │   → 进程内 GstRTSPServer
      │   → 每客户端 appsrc → queue → rtph264pay
      │
      └─ queue
          → videorate：60 FPS 降至 30 FPS
          → mppjpegenc
          → jpegparse
          → uvcsink
```

截至 2026-11-17：

| 项目 | 状态 |
|---|---|
| 自写 `MppEncoderSession` | 已删除 |
| H.264 编码 | `mpph264enc`，已板端验证 |
| MJPEG 编码 | `mppjpegenc`，已板端持续产出 |
| RTSP Server | 进程内 `GstRTSPServer`，已验证 |
| MediaMTX | Live2D 路径已移除 |
| RTSP 首次连接 | 已验证 |
| RTSP 断开重连 | 已验证 |
| 两个 RTSP 客户端并发 | 已验证 |
| H.264 1920×1080@60 | 已验证 |
| MJPEG 约 30 FPS | 板端编码计数已验证 |
| Mac UVC 枚举和抓帧 | 尚未通过 |
| 浏览器播放 | Mac Chrome 经网线直连成功播放和全屏显示 Live2D |
| CPU、RGA 带宽、端到端延迟对比 | 尚未正式测试 |
| 长时间压力测试 | 尚未完成 |

主要设计记录：

- `docs/adr/0006-use-rockchip-gstreamer-mpp-encoders.md`
- `docs/adr/0007-serve-live2d-rtsp-inside-gstreamer.md`
- `docs/adr/0008-add-gstreamer-webrtc-runtime.md`
- `support/gstreamer/README.md`
- `support/gstreamer/build-gstreamer-1.28.9.sh`

### 1.1 WebRTC 运行时补充状态

2026-11-17 已在固定远端环境完成 GStreamer WebRTC 运行时的 ARM64
交叉编译，包括 `webrtcbin`、DTLS、SRTP、SCTP、libnice、OpenSSL、libsrtp、
`webrtcsink` 和 `gstwebrtc-api` 浏览器资源。静态检查确认关键共享库均为
AArch64，并包含 `webrtcbin`、`webrtcsink`、`dtlssrtpenc`、`srtpenc`、
`sctpenc` 与 `mpph264enc` 工厂标识。

RK3588 在 `/opt/vtb/gstreamer-1.28.9-webrtc` 安装了运行时和后来补编的
`libgstnice.so`、`libgstdebugutilsbad.so`，并沿用板端原有 UVC 和 MPP
插件。新版 Live2D 在 `h264parse` 后将同一份 H.264 分发给 RTSP 和
`webrtcsink`。板端已验证插件加载、RTSP 与 WebRTC 监听、`live2d`
视频源注册以及 Mac 请求网页和脚本返回 HTTP 200。Chrome 已显示并全屏播放
`live2d`。本次正常退出时，板端统计提交 37873 帧、H.264 编码 37873 帧、
MJPEG 编码 18951 帧，raw queue 丢帧为 0。相机和相关监听端口均已释放。
浏览器重连、实际解码帧率和端到端延迟待测。

## 2. 最初的问题

最开始提到的“NPP”实际对应 Rockchip MPP：

- RGA：缩放、裁剪、色彩转换和二维图像处理。
- MPP：Rockchip 视频硬件编解码接口。
- NPP：通常指 NVIDIA Performance Primitives，与 RK3588 这条链路无关。

原始实现虽然已经使用 GStreamer 管理推流，但编码部分仍然离开 GStreamer，由项目代码直接调用 MPP：

```text
RGA NV12 DMA-BUF
  → GStreamer appsrc / tee
  → appsink
  → 项目自写 MppEncoderSession
  → 调用 rk_mpi.h
  → 把编码结果复制到新 GstBuffer
  → 第二个 appsrc
  → h264parse / jpegparse
  → RTSP / UVC
```

这带来几类重复工作：

- 应用自己维护 MPP context 和 MppFrame/MppPacket。
- 应用自己导入 DMA-BUF。
- 应用自己处理 GOP、QP、IDR 和编码结果时间戳。
- 编码结果重新复制并送回 GStreamer。
- GStreamer pipeline 被应用层编码回调切成了两段。

调研后确认 `gstreamer-rockchip` 已提供：

- `mpph264enc`
- `mppjpegenc`
- `mpph265enc`
- `mppvideodec`

因此决定把编码职责完整放回 GStreamer pipeline。

## 3. 第一阶段：移除手写 MPP 编码层

### 3.1 选定的插件版本

使用 JeffyCN 的 Rockchip BSP GStreamer 适配插件：

```text
repository: JeffyCN/mirrors
branch:     gstreamer-rockchip
commit:     bff0659a51c13585b3c9c9a68aa8c435beeb3fe1
tree:       faea26be29a4c2ee68d224625723832598488e22
```

需要准确区分：

- `rockchip-linux/mpp` 的 MPP API 和运行库属于 Rockchip 官方实现。
- 当前 `gstreamer-rockchip` 来源是 JeffyCN 维护的 BSP 镜像分支，不能严格称为 Rockchip 官方仓库。
- 这次发现的问题集中在该 GStreamer 适配插件与当前 BSP/MPP 运行时的衔接。

### 3.2 应用层改动

删除：

```text
apps/vtb-live2d/mpp_encoder_session.cpp
apps/vtb-live2d/mpp_encoder_session.hpp
```

同时删除：

- 原始帧 appsink 回调。
- `MppEncodedPacket`。
- 两个编码后 appsrc。
- 应用内 MPP context 创建和释放。
- 应用对 `librockchip_mpp.so` 的直接链接。
- 应用层编码 packet 复制逻辑。

新的 H.264 配置：

```text
mpph264enc
  rc-mode=fixqp
  qp-init=26
  profile=high
  gop=输入 FPS × 2
  header-mode=each-idr
  max-pending=2
  zero-copy-pkt=false
```

MJPEG 配置：

```text
mppjpegenc
  q-factor=80
  qf-min=1
  qf-max=99
  max-pending=2
  zero-copy-pkt=false
```

UVC 60→30 FPS 使用：

```text
videorate drop-only=true
```

### 3.3 DMA-BUF Lease 生命周期

RGA 输出仍然由 `Nv12FrameLease` 管理。应用将 DMA-BUF 包装成 `GstMemory`，并把 Lease holder 绑定到 `GstMemory` 的 qdata。

```text
最后一个 GstMemory 引用释放
  → Nv12FrameLease 析构
  → RGA pool 槽位归还
```

该生命周期覆盖：

- tee 后两个分支同时持有输入。
- `videorate` 产生浅拷贝。
- 编码器异步保留输入 buffer。
- GStreamer queue 暂存 buffer。

## 4. 第一次构建为什么停下

第一次替换完成后，工作实际停在“远端 ARM64 构建成功、尚未部署”。

原因是当时项目规则规定：

- RK3588 SSH 连通默认只授权只读检查。
- 构建授权不自动包含板端部署和服务启动。
- 没有明确授权时不能主动执行端到端推流测试。

当时已经完成：

- 源码调整。
- Doxygen 重新生成。
- GStreamer 1.28.9 交叉构建。
- Rockchip 插件交叉构建。
- `vtb-live2d` ARM64 构建。
- ELF 和 `DT_NEEDED` 检查。

随后等待明确的部署授权。用户再次确认“可以，去做”后，才继续下发 RK3588。这一次属于授权边界停顿，编译器本身没有卡住。

## 5. 远端构建中遇到的问题

### 5.1 旧补丁标记导致构建脚本拒绝继续

复用的 GStreamer 源码目录带有上一任务留下的补丁 hash 标记，而本次补丁集合已经发生变化。

```text
当前补丁集合 hash != 已记录的补丁集合 hash
```

构建脚本在真正编译前主动停止，避免把新旧补丁混合应用。

处理方式：

- 检查每个补丁的正向和反向适用状态。
- 使用任务专属源码目录。
- 保存补丁集合 hash。
- 同一补丁集合可以重复运行。
- 补丁变化时要求使用干净 checkout。

### 5.2 Rockchip MPP include 根目录错误

初版 sysroot 补丁将 include 路径设成：

```text
.../usr/include/rockchip
```

插件源码包含：

```cpp
#include <rockchip/rk_mpi.h>
```

这会查找：

```text
.../usr/include/rockchip/rockchip/rk_mpi.h
```

修正后 include 根改为：

```text
.../usr/include
```

对应补丁为 `support/gstreamer/patches/2001-rockchipmpp-explicit-sysroot.patch`。

### 5.3 板端 MPP 设备权限

板端 `/dev/mpp_service` 当时为：

```text
root:root 0600
```

Rockchip 插件在注册编码器工厂时会探测 MPP 设备。因此普通用户执行：

```bash
gst-inspect-1.0 mpph264enc
```

可能看不到编码器元件。

验证时使用已有的免交互 `sudo` 运行 `gst-inspect` 和正式程序，没有修改设备节点权限。复现时需要区分：

```text
插件文件存在
≠ 插件成功加载
≠ 编码器工厂成功注册
```

### 5.4 LibGodot ABI 不匹配

第一次启动新宿主时复用了旧部署目录的 `libgodot`，程序主动终止：

```text
LibGodot bridge ABI mismatch; rebuild host and library together
```

新宿主需要 ABI 6 的配套 LibGodot。换成同一产品版本、导出 `initialize_v6` 的库后启动成功。该错误会阻止完整应用进入媒体 pipeline。

### 5.5 Shell 展示命令引号错误

部署完成后，一条用于打印配置摘要的 shell 命令存在引号错误。错误发生在复制、校验和前缀切换之后，前面的部署已经完成。随后重新执行只读复核，没有重新覆盖产物。

## 6. 第一轮板端媒体 Bug

### 6.1 冷启动阶段看起来很慢

最初启动后的约 20 秒内，RGA 出现约 0.5～1 秒尖峰，帧率较低。继续观察后：

```text
10 秒增加约 600 帧
```

最终稳定到约 60 FPS。该现象属于冷启动和预热阶段，未形成“越跑越慢”的证据。

### 6.2 RTSP 新客户端运行一段时间后无法解码

现象：

- 编码器输出计数持续增长。
- 老客户端可能仍能接收。
- 新客户端只能拿到 SPS/PPS 或普通 P 帧。
- RTP 抓包没有 NAL type 5 IDR。
- 新客户端缺少随机接入点。

第一步在应用内每两秒向 `mpph264enc` 发送标准 GStreamer ForceKeyUnit：

```cpp
gst_video_event_new_downstream_force_key_unit(...)
```

### 6.3 ForceKeyUnit 没有稳定绑定到具体输入帧

原插件收到 ForceKeyUnit 后调用全局命令：

```text
MPP_ENC_SET_IDR_FRAME
```

插件内部存在输入排队和重试，全局请求无法稳定对应到用户选择的那一帧。补丁改成在具体 `MppFrame` 上写入：

```text
KEY_INPUT_IDR_REQ=1
```

对应补丁为 `2002-rockchipmpp-bind-force-keyframe-to-input-frame.patch`。

### 6.4 读取了错误的 MPP packet 起点

MPP 中的 `MppBuffer` 基址和 `mpp_packet_get_pos(packet)` 含义不同。

插件原实现始终从：

```cpp
mpp_buffer_get_ptr(buffer)
```

读取，而 MPP 后续 packet 的有效数据起点可能已经移动。

结果表现为：

- 第一包 SPS/PPS 位于 buffer 开头，可以正常读取。
- 后续帧的有效数据位于新的 packet position。
- parser 一直读到旧区域。
- 编码器计数不断增加。
- `h264parse` 只看到 SPS/PPS。
- RTSP 出现稀疏帧或空流。

修正方式：

```cpp
pkt_pos = mpp_packet_get_pos(mpkt);
pkt_base = mpp_buffer_get_ptr(mbuf);
pkt_offset = pkt_pos - pkt_base;
```

零复制路径对 `GstMemory` 设置正确 offset；稳定复制路径从 `pkt_pos` 开始复制。对应补丁为 `2003-rockchipmpp-use-packet-valid-data-offset.patch`。

### 6.5 压缩 packet 零复制生命周期不可靠

MPP packet 在 `finish_frame` 后可能释放或复用，异步的 `h264parse`、`jpegparse` 和协议 sink 仍可能持有其内容。

最终配置：

```text
zero-copy-pkt=false
```

由插件内部把压缩后的 H.264/JPEG 数据复制到稳定 `GstBuffer`。该复制发生在压缩数据上，数据量显著小于 1080p NV12 原始帧；应用层仍然不接触 MPP packet。

### 6.6 FFmpeg 重复 DTS 提示

一次 `ffmpeg -f null` 测试出现两次重复 DTS 提示。进一步抓取源 RTP packet 后确认：

- 5 秒共 301 个 packet。
- 首个配置包没有时间戳。
- 其余 PTS/DTS 严格递增。

提示来自 FFmpeg 解码后输出到 `null` 时的时间基量化，没有发现源 RTP 时间戳倒退。

### 6.7 第一阶段最终结果

修复后：

```text
8 秒 RTP：
  404 个访问单元
  3 个 IDR
  401 个普通 slice
  约两秒一个 IDR
```

板端退出统计：

```text
submitted=7628
h264=7567
mjpeg=3823
idr_requests=63
pipeline_errors=0
```

H.264 约 60 FPS，MJPEG 约 30 FPS。

## 7. 第二阶段：移除 MediaMTX，使用进程内 GstRTSPServer

### 7.1 MediaMTX 原来的职责

原链路：

```text
mpph264enc
  → h264parse
  → rtspclientsink
  → MediaMTX :8554
  → RTSP 客户端
  → MediaMTX WebRTC :8889
  → 浏览器
```

`rtspclientsink` 是发布客户端，需要 RTSP Server 接收推流。MediaMTX 当时承担：

- RTSP Server。
- 多客户端分发。
- 客户端断开重连。
- WebRTC 转换。
- 8889 网页播放器。

用户决定统一使用 GStreamer 后，改成进程内 `gst-rtsp-server`。

### 7.2 最终 RTSP 结构

```text
mpph264enc
  → h264parse
  → appsink
  → 应用缓存 caps 和最近 IDR
  → GstRTSPServer
      ├─ client A：appsrc → queue → rtph264pay
      └─ client B：appsrc → queue → rtph264pay
```

关键点：

- MPP H.264 编码器只有一个。
- 每个客户端有独立的 RTP pipeline。
- `gst_buffer_copy()` 只复制 buffer 元数据，默认共享压缩 payload 内存。
- 每个客户端的时间戳从零连续生成。
- 慢客户端有自己的有界 leaky queue。
- 新客户端先注入缓存的最近 IDR。
- 新客户端建立后请求新的 IDR。

### 7.3 为什么没有使用 shared media factory

最初尝试让所有客户端共享同一个 `GstRTSPMedia`，遇到：

- 第一次连接可以工作。
- 客户端断开后 media 被 unprepare。
- 默认 media 无法可靠重新 prepare。
- 第二次 DESCRIBE 返回 503。
- 使用 prepared 门控又产生 appsrc preroll 循环依赖。

最终改为：

```text
factory shared=false
```

每个客户端重新创建：

```text
appsrc → queue → rtph264pay
```

客户端生命周期彼此独立，同时继续复用同一个 H.264 编码器。

## 8. GstRTSPServer 调试过程中遇到的问题

### 8.1 新客户端继承主 pipeline 的大时间戳

主编码 pipeline 已运行很久后，新 RTSP pipeline 才创建。如果直接把原始 PTS/DTS 送进去，新 pipeline 会把它们理解为未来时间并等待。

最终处理：

- 为每个 RTSP 客户端建立独立时间戳基准。
- 复制 buffer metadata。
- 从零开始生成连续 PTS/DTS。
- 每帧间隔约 16.67 ms。

### 8.2 Server 启动早于 H.264 caps

如果 RTSP Server 在第一帧之前就监听 8554，客户端可能在 caps/SPS/PPS 尚未产生时发送 DESCRIBE。

```text
DESCRIBE → 503 Service Unavailable
```

最终改成：

```text
h264parse 产生 caps
并且已经缓存首个 IDR
  → 启动 GstRTSPServer
  → 开始监听 8554
```

因此看到 8554 已监听时，媒体已经具备描述条件。

### 8.3 新客户端 preroll 缺少 IDR

非共享 media 在 DESCRIBE/prepare 阶段需要数据才能完成 preroll。只等待未来 IDR 会造成 503 或长时间等待。

解决办法：

- 缓存最近一个 IDR。
- 新客户端创建 appsrc 后立即注入缓存 IDR。
- 后续帧从新的时间戳基准继续。
- 同时向编码器请求一个新 IDR。

### 8.4 逐帧 NAL 扫描导致吞吐降到约 0.5 FPS

为了判断关键帧，曾加入 `gstcodecparsers` 对每个 H.264 packet 做 NAL 扫描。该操作放在 appsink 回调内，阻塞了整个编码输出路径。

```text
H.264 输出约 0.5 FPS
```

移除扫描后恢复。最终直接使用 pipeline 中已有的 `h264parse` 写入的 `GST_BUFFER_FLAG_DELTA_UNIT` 标志。

该现象容易误判为 MPP 驱动或编码器快速重启故障，实际原因在同步回调中的昂贵解析。

## 9. UVC 启动时覆盖 NV12 DMA-BUF fd

### 9.1 触发条件

同一个 RGA NV12 DMA-BUF 经 tee 同时送给：

- H.264 MPP 编码器。
- JPEG MPP 编码器。

早期怀疑编码分支在释放 MPP buffer 时关闭了另一分支仍使用的 fd。第 9.6 节记录了板端从启动开始的跟踪结果，覆盖发生在编码之前。

内核错误类似：

```text
dma_buf_get fd ... failed(-22)
```

还观察到：

- MPP 导入失败。
- fd 持续增长。
- H.264 输出异常变慢。
- 快速重启后现象更容易暴露。

### 9.2 第一次尝试：给每个 MPP 导入 dup fd

尝试为每个编码器 `dup()` 独立 fd，并使用外部 DMA-BUF 导入。RTSP 功能一度恢复，但进一步检查发现：

- 内核仍持续出现导入错误。
- fd 仍有泄漏。
- 所有权规则依然不清晰。

因此没有把该版本作为最终实现。

### 9.3 曾尝试的绕行：外部 DMA-BUF 复制到编码器自有 MPP buffer

当时尝试的路径：

```text
外部 RGA NV12 DMA-BUF
  → 插件 RGA 同步复制
  → 编码器自己的 MPP buffer
  → MPP 编码
```

每个编码分支拥有独立输入缓冲，避免两个异步 MPP context 共享外部 fd。

对应补丁：

- `2004-rockchipmpp-copy-external-dmabuf.patch`
- `2005-rockchipmpp-duplicate-rga-input-fd.patch`
- `2006-rockchipmpp-make-rga-conversion-synchronous.patch`
- `2007-rockchipmpp-import-rga-buffer-handles.patch`

### 9.4 为什么还需要 2007

只使用旧式：

```text
wrapbuffer_fd_t(fd)
```

时，librga 可能把裸 fd 延迟交给内核。在当前 BSP 上，提交前 fd 数字仍可能失效。

项目已有 RGA 路径长期使用：

```text
importbuffer_fd()
wrapbuffer_handle_t()
releasebuffer_handle()
```

插件最终也采用同样方式：

1. `dup()` 输入 fd。
2. `importbuffer_fd()` 导入 source handle。
3. 导入 destination handle。
4. 使用 `wrapbuffer_handle_t()` 构造 RGA buffer。
5. `IM_SYNC` 同步执行。
6. 释放两个 handle。
7. 关闭复制出的 fd。

当时的短时板端观察：

```text
fd 在 76～81 波动
没有单向增长
没有新增 DMA-BUF/RGA/MPP 内核错误
```

### 9.5 当前的性能代价

该历史方案包含以下复制：

- 外部 NV12 对每个编码分支增加一次 RGA 硬件复制。
- 压缩后的 H.264/JPEG packet 使用稳定内存复制。
- RTSP 多客户端仍共用一次 H.264 编码，不增加 MPP 编码器。

这次改造的主要价值：

- 删除应用层手写 MPP 编码代码。
- 统一 GStreamer buffer/event/EOS/error 管理。
- 明确 DMA-BUF 所有权。
- 移除 MediaMTX 进程。
- 避免网络客户端数量增加硬件编码器数量。

正式 CPU、RGA 带宽和端到端延迟还需要对照测试。

### 9.6 板端启动跟踪与修复

在 `uvcsink` 启动时捕获到 `dup3(52</dev/video2>, 53</dmabuf>, O_CLOEXEC)`。
调用前 fd 53 对应 NV12 池槽位，调用后 fd 53 指向 `/dev/video2`。RGA 已导入的
handle 仍保活原缓冲区，随后提交给 GStreamer 的 fd 却指向 UVC 设备。

补丁 0006 将 `gst_uvc_sink_watch()` 中的固定目标复制改为
`fcntl(device_fd, F_DUPFD_CLOEXEC, 0)`。临时补丁 2004 已移除，布局匹配的 NV12 恢复
MPP 直接导入。2005–2007 仅用于需要 RGA 转换的回退路径。

修复后启动跟踪记录 `fcntl(52</dev/video2>, F_DUPFD_CLOEXEC, 0) = 57</dev/video2>`，
NV12 池的 fd 53–56 仍指向四块 DMA-BUF。板端编码日志记录 H.264 直导 4,884 帧、MJPEG
直导 2,446 帧，两路的 RGA 或 CPU 拷贝回退均为零。Mac 客户端解码 301 帧
1920×1080@60 H.264。新版插件已部署到原 GStreamer 1.28.9 目录，生产进程使用常规日志级别。
本次 MJPEG 验证覆盖板端编码输出，Mac USB 主机枚举和抓帧另行验证。

## 10. 完整补丁表

| 补丁 | 作用 |
|---|---|
| 2001 | 允许显式指定 Rockchip MPP sysroot |
| 2002 | ForceKeyUnit 绑定到具体输入 MppFrame |
| 2003 | 使用 `mpp_packet_get_pos()` 的有效数据范围 |
| 0006 | UVC 事件监听占用空闲 fd，修复覆盖池槽位 |
| 2004（已移除） | 早期强制外部 DMA-BUF 走拷贝的绕行 |
| 2005 | RGA 转换期间用 `dup()` 保持 source fd |
| 2006 | RGA 转换增加 `IM_SYNC` |
| 2007 | 使用 `importbuffer_fd()` 和 RGA handle 固定缓冲所有权 |

构建脚本会：

- 检查插件 commit。
- 检查补丁集合 hash。
- 按词法顺序应用各组现有补丁。
- 补丁已应用时允许重复构建。
- 补丁集合变化时拒绝继续复用旧源码目录。

## 11. 可复现构建环境

### 11.1 源码状态

当时基础版本：

```text
branch: main
commit: 86f9a03890968347c9abce89f2299d60adff598f
```

相关修改仍处于未提交状态。只 checkout 上述 commit 不会得到最终代码和补丁。

复现前必须同时具备：

- 当前任务修改后的工作区。
- `support/gstreamer/patches/2001`～`2007`。
- 修改后的 GStreamer 构建脚本。
- 修改后的 `gstreamer_media_session.cpp`。
- 删除 `mpp_encoder_session.cpp/.hpp` 的状态。

当前工作区还包含其他用户改动，因此不能把整个 dirty diff 不加筛选地当成单一补丁发布。

### 11.2 编译服务器

```text
host: fx@10.191.32.121
architecture: x86_64
target: AArch64
compiler: Buildroot/Godot GCC 13.2.0
```

固定工具链：

```bash
VTB_NATIVE_ROOT=/hdd/.fx/toolchains/openvt-dmabuf-rga-mpp-20261027-01
VTB_SDK="$VTB_NATIVE_ROOT/sdk"
VTB_ROCKCHIP_SYSROOT="$VTB_NATIVE_ROOT/board-sysroot"
```

### 11.3 GStreamer 运行时构建

准备任务专属目录后：

```bash
export GSTREAMER_SOURCE_ROOT=/task/gstreamer-source
export GSTREAMER_BUILD_ROOT=/task/gstreamer-build
export GSTREAMER_STAGING_ROOT=/task/gstreamer-sysroot
export GSTREAMER_MESON_PYTHON=/usr/bin/python3
export GSTREAMER_MESON_PYTHONPATH=/task/build-tools/meson-1.7.2
export GSTREAMER_HOST_TOOLS=/task/host-tools/glib/usr/bin
export GSTREAMER_GUDEV_ROOT=/task/build-tools/gudev-root
export GSTREAMER_ROCKCHIP_SOURCE=/task/gstreamer-rockchip
export VTB_GODOT_SDK=/hdd/.fx/toolchains/openvt-dmabuf-rga-mpp-20261027-01/sdk
export VTB_ROCKCHIP_SYSROOT=/hdd/.fx/toolchains/openvt-dmabuf-rga-mpp-20261027-01/board-sysroot
export GSTREAMER_BUILD_JOBS=24

./support/gstreamer/build-gstreamer-1.28.9.sh
```

源码包、Meson、GUdev 和 GLib host tools 的固定 SHA-256 已记录在 `support/gstreamer/README.md`。

### 11.4 实际应用构建配置

实际验证使用的主要配置为：

```bash
cmake -S "$TASK/source" -B "$TASK/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER="$SDK/bin/aarch64-godot-linux-gnu-g++" \
  -DVTB_ENABLE_RTSP_OUTPUT=OFF \
  -DVTB_ENABLE_UVC_OUTPUT=OFF \
  -DVTB_ENABLE_UAC2_OUTPUT=OFF \
  -DVTB_ENABLE_ROCKCHIP_MPP_PUBLISHER=OFF \
  -DVTB_ENABLE_RKNN_FACE=ON \
  -DVTB_ENABLE_RGA_FACE=ON \
  -DVTB_ENABLE_OPENCV_FACE_PIPELINE=ON \
  -DVTB_ENABLE_EMBEDDED_GODOT=ON \
  -DVTB_ROCKCHIP_SYSROOT="$ROCKCHIP" \
  -DVTB_RKNN_SYSROOT="$RKNN" \
  -DOpenCV_DIR="$OPENCV/usr/lib/aarch64-linux-gnu/cmake/opencv5" \
  -DVTB_GODOT_SOURCE_ROOT="$GODOT_SOURCE" \
  -DVTB_LIBGODOT_LIBRARY="$LIBGODOT" \
  -DVTB_GSTREAMER_ROOT="$GST" \
  -DVTB_EMBEDDED_RPATH_LINK="$RPATH_LINK"

cmake --build "$TASK/build" --target vtb-live2d -j 24
```

这里的 `VTB_ENABLE_RTSP_OUTPUT` 和 `VTB_ENABLE_UVC_OUTPUT` 是历史独立 publisher 开关。当前嵌入式 GStreamer 媒体会话由 `VTB_ENABLE_EMBEDDED_GODOT` 路径构建，所以经过验证的命令中这些旧开关为 `OFF`。

### 11.5 产物检查

```bash
file "$TASK/build/vtb-live2d"
readelf -h "$TASK/build/vtb-live2d"
readelf -d "$TASK/build/vtb-live2d" | grep NEEDED
readelf -d libgstrockchipmpp.so | grep NEEDED
```

预期：

- `vtb-live2d` 为 ELF64 AArch64。
- `vtb-live2d` 不直接依赖 `librockchip_mpp.so`。
- `libgstrockchipmpp.so` 依赖 `librockchip_mpp.so.1`。
- 插件包含 `mpph264enc` 和 `mppjpegenc` 工厂。

## 12. 板端部署与运行检查

最终部署位置：

```text
应用：
/home/huangnv/VTB/runtime/.gstreamer-mpp-deploy-20261116-01/vtb-live2d

插件：
/opt/vtb/gstreamer-1.28.9/lib/aarch64-linux-gnu/gstreamer-1.0/libgstrockchipmpp.so

RTSP：
rtsp://192.168.77.2:8554/live2d
```

部署前应：

1. 校验 ARM64 架构。
2. 校验 SHA-256。
3. 保留旧应用和旧插件副本。
4. 使用新的 `GST_REGISTRY`，避免复用旧插件缓存。
5. 确认 MediaMTX 已停止。
6. 确认 8554 没有被其他进程占用。
7. 确认运行用户能访问 MPP、RGA 和 DMA heap 设备。

启动环境需要固定：

```text
LD_LIBRARY_PATH
GST_PLUGIN_PATH_1_0
GST_PLUGIN_SYSTEM_PATH_1_0
GST_REGISTRY
```

必须确保同一套 GStreamer 1.28.9 的库和插件同时被加载，避免混用板端系统 GStreamer 1.24.2。

## 13. 验证命令和验收标准

### 13.1 插件注册

```bash
gst-inspect-1.0 mpph264enc
gst-inspect-1.0 mppjpegenc
gst-inspect-1.0 videorate
```

### 13.2 RTSP 首次连接

```bash
ffmpeg \
  -rtsp_transport tcp \
  -i rtsp://192.168.77.2:8554/live2d \
  -t 5 \
  -f null -
```

要求：

- 5 秒内开始解码。
- 分辨率 1920×1080。
- 帧率接近 60 FPS。
- 无持续 PPS、参考帧或解码错误。

### 13.3 重连

```bash
ffprobe \
  -v error \
  -rtsp_transport tcp \
  -read_intervals '%+5' \
  -count_packets \
  -select_streams v:0 \
  -show_entries stream=codec_name,width,height,r_frame_rate,nb_read_packets \
  -of default=noprint_wrappers=1 \
  rtsp://192.168.77.2:8554/live2d
```

最终一次结果：

```text
codec_name=h264
width=1920
height=1080
r_frame_rate=60/1
nb_read_packets=301
```

### 13.4 双客户端

同时启动两个 TCP RTSP 客户端，各解码 5 秒。

要求：

- 两个客户端均能完成解码。
- MPP H.264 编码器数量不增加。
- 慢客户端不阻塞另一个客户端。
- 应用无 pipeline error。

### 13.5 fd 稳定性

重复观察：

```bash
find /proc/$PID/fd -mindepth 1 -maxdepth 1 | wc -l
```

最终通过区间：

```text
76～81
```

允许缓冲和客户端连接带来的波动，不能持续单向增长。

### 13.6 内核日志

检查新启动区间：

```bash
dmesg | grep -E \
'dma_buf_get|failed to map|mpp_dma|rga.*failed|vcodec.*error|no memory region'
```

最终通过区间没有新增相关错误。

### 13.7 正常退出统计

最终一次：

```text
submitted=3921
h264=3921
mjpeg=1962
rtsp_pushed=1251
raw_dropped=0
```

这对应 H.264 约 60 FPS、MJPEG 约 30 FPS、raw queue 无丢帧。

## 14. 最终产物哈希

最终可复现产物：

```text
b145977792d1d54a7aefe577d8b44c3d9b639aacb0e31e9f0ea01caf52798602
  vtb-live2d-embedded-rtsp-final

a4679e3f4c2ec3518b25c1d702b2a25f0feb8152715b5398efe42fdf6aede5a3
  libgstrockchipmpp-rga-handles-reproducible.so
```

远端保留目录：

```text
/hdd/.fx/vtb-builds/gstreamer-mpp-20261116-5482Ng
/hdd/.fx/vtb-builds/embedded-rtsp-20261117-oKJw6d
```

其中包含构建日志、manifest、`SHA256SUMS`、ARM64 应用和最终 Rockchip 插件。

## 15. 为什么浏览器之前能播放，现在打不开

以前浏览器访问的是：

```text
http://192.168.77.2:8889/live2d/
```

实际链路：

```text
GStreamer RTSP 发布
  → MediaMTX
  → WebRTC
  → 8889 网页播放器
  → 浏览器
```

当前地址：

```text
rtsp://192.168.77.2:8554/live2d
```

是纯 RTSP。Chrome、Safari、Edge 通常没有原生 RTSP 播放能力。

当前可以使用：

- VLC
- IINA
- ffplay
- FFmpeg
- GStreamer 客户端

GStreamer 自身支持 WebRTC，可使用：

- `webrtcbin`
- `webrtcsink`
- `whepserversink`

新版 Live2D 已把编码分支接到 `webrtcsink`。直连 Mac 可访问
`http://192.168.77.2:8080/`，页面通过 8443 连接信令服务。Mac Chrome 已验证
点击 Remote streams 中的 `live2d` 后可播放并切到全屏。

## 16. 仍未完成的项目

当前可以确认：

- 应用层手写 MPP 编码已经移除。
- Rockchip GStreamer 编码插件已经工作。
- 进程内 RTSP Server 已工作。
- 首连、重连、双客户端、解码和资源稳定性已通过短时测试。

仍需单独完成：

1. Mac UVC 枚举、打开、抓帧、关闭和重新打开。
2. 一小时以上持续运行。
3. CPU、内存、温度和 fd 趋势。
4. RGA 带宽占用。
5. 与旧 `MppEncoderSession` 的端到端延迟对比。
6. 客户端数量上限。
7. 浏览器 WebRTC 重连、解码帧率和端到端延迟。

本文反映的是 2026-11-17 的实际构建和板端验证状态，没有把尚未执行的
WebRTC 重连、UVC 主机验收或性能对比写成已完成。
