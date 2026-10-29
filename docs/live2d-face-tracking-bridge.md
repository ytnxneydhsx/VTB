# OpenSeeFace 到 Live2D 参数桥接

## 目标与设计理由

当前 OpenSeeFace/RKNN 链路已经计算人脸姿态、眼睛、嘴部和关键点，但只把
黑底关键点画布交给编码端。Godot/Ayagami 链路已经能接收模型参数，但参数来自
固定测试动画。本桥接层把两端已有能力连接起来：

```text
V4L2 摄像头
  → OpenSeeFace/RKNN
  → Live2D 标准参数快照
  → 本机 UDP（latest-only）
  → Godot/Ayagami
  → DMA-BUF → RGA → MPP H.264
```

这项改动解决参数契约、坐标校准、限幅、平滑、时间戳和丢失跟踪恢复。它增加
一个本机 UDP 数据报，不宣称提高推理、渲染或编码吞吐量。

更简单的方案是在 Godot 脚本中直接读取 66 点并计算所有参数。当前没有采用，
因为 OpenSeeFace 已经给出 Euler 角、左右眼开合和经过个体自适应的嘴部特征，
重复计算会产生两套语义和校准规则。映射代码保留在 VTB 自有目录，不修改
`thirdparty/ayagami*`。

## 数据流和文件

- `tools/live2d_face_tracking.py`：校准、参数映射、时间平滑、协议编码和 UDP 发送。
- `tools/openseeface_3d_preview.py`：在每次 `tracker.predict()` 后选择最新人脸并发送快照。
- `experiments/godot/openvt-dmabuf-capture/live2d_tracking_receiver.gd`：校验协议、读空队列并选择最新数据报。
- `experiments/godot/openvt-dmabuf-capture/render_dmabuf_video.gd`：把快照写入存在于模型中的参数；过期时回到模型默认值。

摄像头线程原有的单槽 latest-frame 策略保持不变。UDP 接收端同样采用
latest-only 策略，不补算过期表情帧。

摄像头驱动缓冲池与推理交接槽分别配置：`--capture-buffers` 默认请求 4 个
V4L2 缓冲，保证应用持有当前帧时驱动仍有空闲缓冲可写；`LatestFrameCapture`
继续只保留一个最新帧，推理落后时覆盖旧帧，避免延迟持续累积。启动脚本可用
`VTB_FACE3D_CAPTURE_BUFFERS` 覆盖驱动缓冲请求值，低于 2 的值会被拒绝。

## 映射规则

启动后的前 30 个有效跟踪帧用于计算中立头姿中位数。用户应在这段时间正视
摄像头并保持自然表情。

| OpenSeeFace | Live2D | 当前规则 |
|---|---|---|
| `euler[1] - neutral_yaw` | `ParamAngleX` | 限制到 ±30°，方向可反转 |
| `euler[0] - neutral_pitch` | `ParamAngleY` | 限制到 ±30°，方向可反转 |
| `euler[2] - neutral_roll` | `ParamAngleZ` | 限制到 ±30°，方向可反转 |
| 头部角度 | `ParamBodyAngleX/Y/Z` | 乘以 0.3 |
| `eye_blink[1]` | `ParamEyeLOpen` | 限制到 0～1 |
| `eye_blink[0]` | `ParamEyeROpen` | 限制到 0～1 |
| `current_features.mouth_open` | `ParamMouthOpenY` | 可配置端点，归一化到 0～1 |
| `current_features.mouth_wide` | `ParamMouthForm` | 限制到 -1～1 |

OpenSeeFace 的原始 Euler 角使用 OpenCV 坐标。第一次实机验证必须分别执行左转、
右转、抬头、低头、左倾和右倾，确认三个参数的符号。需要反向时使用对应的
`--live2d-angle-*-sign -1`，无需修改代码。

映射结果采用 50 ms 时间常数的一阶低通滤波。该算法根据实际时间间隔计算权重，
推理帧率变化时不会改变滤波时间常数。模型写入前还会依据模型公开的参数范围限幅。

## UDP 协议 v1

默认目的地址为 `127.0.0.1:11574`。每个 JSON 数据报包含：

```json
{
  "protocol": "vtb-live2d-parameters",
  "version": 1,
  "session_id": 1,
  "frame_id": 1,
  "capture_sequence": 1,
  "capture_ready_ns": 0,
  "points_ready_ns": 0,
  "sent_ns": 0,
  "tracking_valid": true,
  "calibrated": true,
  "confidence": 0.9,
  "view": {
    "zoom": 1.0,
    "offset_x": 0.0,
    "offset_y": 0.0
  },
  "parameters": {
    "ParamAngleX": 0.0,
    "ParamEyeLOpen": 1.0,
    "ParamMouthOpenY": 0.0
  }
}
```

Python 时间戳来自 RK3588 的系统单调时钟，Godot `Time.get_ticks_usec()` 从引擎
启动时开始计时，两者原点不同。接收端使用 `收包 ticks - sent_ns` 建立时钟偏移，
并持续保留观测到的最小偏移，以排除排队时间。`points_ready_ns` 加该偏移后才能与
Godot 当前 ticks 计算参数年龄。该方法要求发送端和接收端运行在同一台设备；默认
回环地址满足这一条件。Godot 只接受协议白名单内的十个参数。若把监听地址改为
非回环接口，除了 UDP 没有认证或加密保护，还需要重新设计跨主机时钟同步和网络
信任边界。

## RK3588 启动方式

先启动 Godot 接收端：

```bash
OPENVT_LIVE2D_TRACKING_PORT=11574 \
OPENVT_DMABUF_OUTPUT_DIR=/home/huangnv/openvt-live2d-tracking-run-01 \
./experiments/godot/openvt-dmabuf-capture/run_render_on_rk.sh
```

在另一个终端启动 OpenSeeFace/RKNN 参数发送端：

```bash
build/openseeface-venv/bin/python tools/openseeface_3d_preview.py \
  --capture /dev/video0 \
  --width 1280 \
  --height 720 \
  --fps 60 \
  --backend rknn \
  --rknn-model-dir models/openseeface \
  --live2d-udp 127.0.0.1:11574 \
  --no-window
```

首次物理摄像头和 RK3588 运行需要单独授权。当前实现阶段没有执行这些命令。

常用校准选项：

```text
--live2d-calibration-frames 30
--live2d-smoothing-ms 50
--live2d-min-confidence 0.4
--live2d-max-pnp-error 100
--live2d-angle-x-sign -1
--live2d-angle-y-sign -1
--live2d-angle-z-sign -1
--live2d-mouth-closed 0.0
--live2d-mouth-open 1.0
```

## 手动重新校准

用户可能在启动时没有正视摄像头，导致自动采集的中立头姿与实际正脸
不一致。重启整个 OpenSeeFace 进程也能重新校准，但会中断摄像头和
`/face3d` 视频流。当前实现在同一 OpenSeeFace 进程内增加一个最小 HTTP
控制口，不改变视频帧、Live2D 参数和 RTSP/WebRTC 的数据路径。

```text
POST /recalibrate
  → 清空原中立姿态
  → Live2D 头部参数回到默认值
  → 采集接下来 30 个有效正脸帧
  → 取三个 Euler 轴的中位数作为新零点
```

`GET /health` 返回 `state`、`calibrated`、`tracking_valid`、`confidence`、
`progress` 和 `required`，Mac 监看页用它显示校准进度。默认绑定
`192.168.77.2:11575`，并只为 `http://192.168.77.1:8900` 返回浏览器跨域
许可。该接口没有认证和加密，只用于 Mac↔RK 直连本地网段，不应暴露
到不可信网络。

## 交互式高精度取景

Mac 双流监看页允许在右侧 Live2D 区域滚轮缩放、拖动平移、双击恢复全身，
也提供全身、半身和面部预设。交互更新通过现有控制口发送：

```text
POST /view
  → OpenSeeFace 控制状态
  → 现有 Live2D UDP 快照的可选 view 字段
  → Godot model.scale / model.position
  → 1920×1080 SubViewport 重新渲染
  → DMA-BUF → RGA → MPP → WebRTC
```

`view.zoom` 是相对全身适配比例，范围为 `0.5～6.0`；`offset_x` 和
`offset_y` 是相对 1920×1080 输出宽高的平移量，范围为 `-2.0～2.0`。
取景在 Live2D 网格栅格化之前应用，浏览器只发送控制值，没有对已编码视频做
CSS 放大或像素后处理。当前模型使用 8192×8192 纹理图集，因此面部填满 1080p
输出时能利用更多源纹理像素；最终可见细节仍受图集局部纹素密度和 1080p 输出
分辨率限制。

`view` 是 UDP v1 的可选扩展。旧发送端省略该字段时，Godot 保持全身默认取景；
接收端拒绝非有限数值和越界控制值。控制口沿用原来的局域网来源限制，不增加
新的服务或媒体跳数。

验收时确认单次点击只产生一次重校准请求，进度能从 `0/30` 到
`30/30`，校准前后两路视频保持在线，用户正视时 `ParamAngleX/Y/Z`
回到接近 `0°`。

## 性能打点

性能统计保留两类数据。`session_id + frame_id` 逐帧时间戳继续负责跨 Python、
Godot 和 C++ 的帧关联；`vtb::instrumentation` 负责 C++ 进程内阶段的累计统计。
这样可以保留现有诊断页所需的逐帧 trace 和 P50/P95/P99，同时统一 C++ 阶段的
count、latest、min、average 和 max。Python、GDScript 和 Rust 使用各自的单调
时钟输出同名阶段，当前不增加语言绑定层。

OpenSeeFace/RKNN 进程按 300 个处理帧输出一次有界窗口统计：

```bash
VTB_FACE3D_PERFORMANCE_STATS=1 \
VTB_FACE3D_PERFORMANCE_REPORT_FRAMES=300 \
VTB_FACE3D_LIVE2D_UDP=127.0.0.1:11574 \
./scripts/run-face3d-browser-stream.sh
```

汇总以 `VTB_PERF_SUMMARY` JSON 单行写入 stderr，覆盖采集线程交接、OpenSeeFace
预测、RKNN detector/landmark、Live2D 映射、JSON 编码和 UDP `sendto()`。
timed-frame 与 UDP 参数包使用同一个 `session_id`，协议版本和字段布局保持不变。

Godot/Ayagami/媒体输出进程通过以下开关启用：

```bash
OPENVT_PROFILE=1 \
OPENVT_PROFILE_REPORT_FRAMES=300 \
OPENVT_LIVE2D_TRACKING_PORT=11574 \
OPENVT_LIVE2D_TOTAL_FRAMES=0 \
./experiments/godot/openvt-dmabuf-capture/run_render_on_rk.sh
```

这个开关同时启用 GDScript 跟踪统计和 `OPENVT_AYAGAMI_PROFILE=1`。日志包含 UDP
接收/解析、参数年龄、参数应用、源帧复用与覆盖、Ayagami 顶点提交、GPU copy、
媒体队列、RGA、MPP、`OutputManager::publish()` 调用以及 `RtspOutput::write()`。
C++ DMA-BUF 分位数只保留最近 4096 个样本，累计 count/average/min/max 由
`vtb::instrumentation` 保存，持续推流不会按帧无限增长样本数组。

这些名称刻意区分边界：`capture_ready` 从 OpenCV `capture.read()` 返回后开始，
不包含曝光、USB 传输和 MJPEG 解码前半段；`output_publish_call` 只表示有界队列
入队调用；`rtsp_write` 表示 libavformat 写调用，仍不等同于浏览器呈现时间。
浏览器端到端延迟继续由逐帧标记和 presentation 匹配验证。

## 边界和验证指标

Godot DMA-BUF 模块现在把 MPP H.264 编码包直接封装成 `MediaPacket`，并通过现有
`OutputManager + RtspOutput` 发布到 RTSP/TCP。模块不再创建 Annex-B 输出文件；
SPS/PPS、单调时钟 PTS/DTS、帧序号和关键帧标记随输出链路传递。该实现仍需在
RK3588 上完成 MediaMTX 和客户端连续解码验证，UVC 输出尚未接入本模块。

实机验证至少记录：

- 摄像头取帧到参数生成的 P50/P95；
- 参数生成到 Godot 应用时的数据年龄；
- 映射耗时和 UDP 丢包数；
- 输出 FPS、队列深度和丢帧数；
- 捕获到 RTSP/WebRTC 显示的端到端 P50/P95；
- 丢脸后的回中立时间和重新捕获时间；
- 三个头姿轴方向、眼睛左右顺序、嘴部开合范围和参数越界次数。

只有测量发现 JSON 编解码成为瓶颈时，才把协议替换为固定二进制记录。当前每帧
只有十个浮点参数，JSON 便于首次联调和日志复核。
