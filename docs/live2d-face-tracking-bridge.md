# C++ 人脸跟踪到 Live2D 同进程链路

## 当前结构

实时 Live2D 产品路径由 `vtb-live2d` 主程序统一编排。摄像头、RKNN、几何估计和
参数映射位于 LibGodot 外部；Godot 只接收已经生成的 Live2D 参数帧：

```text
V4L2 CameraSource (MMAP / DQBUF / QBUF)
  → capacity-one LatestFrameSlot
  → VideoFrameConsumer
  → FaceFrameConsumer
  → RKNN detector (NPU core 0)
  → RKNN 66-point landmark model (NPU core 1)
  → OpenSeeFace-compatible C++ PnP / expression features
  → optional latest-only FaceDiagnosticSession → NV12 DMA-BUF → MPP H.264
      → host MediaOutputSession → OutputManager → /face3d RTSP
  → C++ Live2DFaceMapper
  → Live2DTrackingSession capacity-one parameter snapshot
  → openvt_libgodot_application_submit_live2d_frame()
  → Godot main thread parameter input
  → Ayagami model parameters
  → SubViewport → DMA-BUF → RGA → MPP H.264
  → EncodedVideoSink 同步编码包回调
  → 宿主 MediaOutputSession → OutputManager → RTSP
```

Godot/Ayagami、DMA-BUF、RGA 和 MPP 保留原有资源链；输出协议移到宿主。`vtb-live2d` 可执行文件
直接链接 V4L2 视频输入、OpenCV PnP 几何计算、RKNN 和 `Live2DTrackingSession`；LibGodot 共享库不再编译
摄像头、RKNN 模型、几何估计、人脸管线、OutputManager 或 RTSP 源码，也不链接 FFmpeg。

## 模块边界

- `CameraSource`：直接打开 V4L2 摄像头，管理 MMAP/DMA-BUF 缓冲区和
  `DQBUF/QBUF` 帧租约，当前输出 YUYV/NV12/NV21 原始帧。
- `VideoProcessingRuntime`：只组合 `VideoSource`、`LatestFrameSlot` 和
  `VideoFrameConsumer`，管理采集/消费线程、latest-only 覆盖、停止和异常传播。
- `FaceFrameConsumer`：把统一帧消费接口适配到 `FacePipeline` 并发布人脸结果；结果
  可由 Live2D mapper 与可选诊断观察者同时读取。
- `RknnFaceDetector` / `RknnLandmarkEstimator`：加载 `.rknn` 模型并固定到两个 NPU core。
- `OpenSeeFaceGeometryEstimator`：保留 70 点模板、14 点 PnP、Euler 角、66 点反投影和
  Remedian 表情特征语义。
- `Live2DFaceMapper`：中立姿态校准、限幅、参数映射和按单调时钟计算的一阶平滑。
- `Live2DTrackingSession`：应用层组合类，创建 `FaceFrameConsumer`，管理参数快照和校准。
- `Live2DControlServer`：独立的 HTTP 控制组件，借用寿命更长的会话，序列化状态和转发命令。
- `MediaOutputSession`：宿主编码包接收器，复制回调中的借用字节，管理输出启动、排空和停止。
- `FaceDiagnosticSession`：只复制最新 `FaceTrackingResult`，后台绘制黑底人脸框和
  66 点 NV12 画面，并通过独立 MPP 编码器交付 `/face3d` 编码包。
- `VtbApplication`：分别创建视频输入与模型管线，管理顶层启动、停止和错误传播。
- `GodotRuntimeThread`：专用所有者线程；在该线程内创建、驱动和销毁 LibGodot，并
  读取最新参数快照提交给 Godot。
- `Live2DParameters`：独立的十参数值对象；Godot 只编译这个数据类型，不编译
  `Live2DFaceMapper`。
- `OpenVTLive2DFrameInput`：宿主与 LibGodot 之间的固定数值帧对象，只包含十个
  Live2D 参数、取景状态、结果产生时间戳和 snapshot_generation；通过 C linkage 函数提交。
- `EmbeddedLive2DApplication`：Godot 内部渲染/编码编排，只消费参数帧并驱动 Ayagami。

推理回调不访问 Godot 对象。它只更新纯 C++ mailbox；Godot 所有者线程通过固定桥接函数
提交数值字段，跨边界不再传递 Godot `Dictionary`、摄像头对象或模型对象。

## 参数映射

启动后的前 30 个有效帧用于计算中立头姿中位数：

| C++ 跟踪结果 | Live2D 参数 | 规则 |
|---|---|---|
| yaw - neutral yaw | `ParamAngleX` | 默认限幅 ±30° |
| pitch - neutral pitch | `ParamAngleY` | 默认限幅 ±30° |
| roll - neutral roll | `ParamAngleZ` | 默认限幅 ±30° |
| 三个头部角度 | `ParamBodyAngleX/Y/Z` | 乘以 0.3 |
| 左右眼自适应特征 | `ParamEyeLOpen/ROpen` | 限幅 0～1 |
| `mouth_open` | `ParamMouthOpenY` | 归一化到 0～1 |
| `mouth_wide` | `ParamMouthForm` | 限幅 -1～1 |

`read_snapshot_if_changed()` 仅供 Godot 所有者线程消费；空值表示没有新快照。`snapshot_generation`
随映射或取景变化递增，不能当作摄像头帧号；来源帧号保留在映射结果的 `frame_id()`。
取景更新保持原映射时间戳。没有新快照时继续使用最近参数，不能立即触发衰减。

映射结果默认使用 50 ms 时间常数。距结果产生时间超过 150 ms 后，Godot 内部的
`EmbeddedLive2DApplication` 以 250 ms 时间常数把模型恢复到默认值。

## 启动

运行脚本启动一个 `vtb-live2d` 进程，Godot 作为进程内 LibGodot 实例运行：

```bash
OPENVT_CONFIG=/path/to/deployment/config/vtb-live2d.json \
./experiments/godot/embedded-runtime/run_on_rk.sh
```

配置结构见 [`video-input-configuration.md`](video-input-configuration.md) 和
[`config/vtb-live2d.example.json`](../config/vtb-live2d.example.json)。启动时先通过
V4L2 确认设备类型和采集能力，并要求驱动回填的 FourCC、尺寸、帧率与 JSON 请求
完全一致，然后直接使用
MMAP 缓冲区采集。当前产品配置只接受 `v4l2_camera` 输入和
YUYV/NV12/NV21 原始格式。

## 控制口

同一个 C++ 进程默认监听 `192.168.77.2:11575`：

- `GET /health`：返回校准、跟踪置信度、校准进度和取景状态。
- `POST /recalibrate`：请求推理线程在下一次更新前清空中立姿态。
- `POST /view`：更新 `zoom`、`offset_x` 和 `offset_y`。

绑定地址、端口和浏览器来源由 JSON 的 `control` 段提供。控制口没有认证或加密，
只适用于受信的直连本地网段。

## 人脸诊断输出

`face_diagnostic_output.enabled=true` 时，`vtb-live2d` 同时发布 `/face3d` 和
`/live2d`。诊断分支接收已经完成推理的值对象，不重新打开摄像头、不重复执行 RKNN，
也不保存 `DecodedFrameLease`。后台编码跟不上时只覆盖尚未消费的旧结果；
`maximum_fps` 只限定诊断输出，Live2D 渲染帧率仍由 `render.output_fps` 控制。

## 构建依赖

主程序的 CMake 构建需要 OpenCV core/calib 与 RKNN sysroot。OpenCV 只用于
PnP 几何计算，摄像头采集不链接 `videoio`：

```bash
export OPENVT_VTB_SOURCE_ROOT=/path/to/VTB
export OPENVT_BOARD_SYSROOT=/path/to/board-sysroot
export OPENVT_FFMPEG_SYSROOT=/path/to/ffmpeg-sysroot
export OPENVT_OPENCV_ROOT=/path/to/arm64-opencv-sysroot
export OPENVT_RKNN_ROOT=/path/to/arm64-rknn-sysroot
```

Godot 内置模块只需要 `OPENVT_VTB_SOURCE_ROOT`、`OPENVT_BOARD_SYSROOT`。OpenCV、
RKNN 和 FFmpeg 由宿主链接。产品构建启用 `VTB_ENABLE_EMBEDDED_GODOT`、
`VTB_ENABLE_OPENCV_FACE_PIPELINE`、`VTB_ENABLE_RKNN_FACE`、`VTB_ENABLE_RGA_FACE`、
`VTB_ENABLE_RTSP_OUTPUT`。两个推理后端关闭时，通用会话与消费适配仍可独立编译。

ABI v3 使用 `openvt_libgodot_application_initialize_v3()`、版本查询和配置尺寸校验，
并把媒体指标 JSONL 路径和报告周期从宿主传入 LibGodot。
宿主和 LibGodot 必须成对构建、成对部署。回调契约与关闭顺序见 [模块边界](module-boundaries.md)。

当前 60 FPS 输出路径保留 30 Hz 摄像头和人脸推理。LibGodot 主线程在应用参数前，
使用全局单调时钟时间戳对最近两个 `Live2DParameters` 快照进行线性插值；当快照周期
没有明显长于渲染周期时直接采用最新值，因此原有 30 Hz 跟踪 / 30 FPS 输出语义不变。
插值采用一个源快照周期的缓冲，30 Hz 输入约增加 33 ms 参数延迟。取景控制立即生效，
新鲜度仍按最新真实快照的产生时间计算，插值帧不会续期，超时后继续执行平滑回默认值。

2026-10-30 的拆分前路径已在 RK3588 完成实机验证。摄像头路径连续处理 300 个渲染帧，
MPP 编码和 RTSP 写出均为 300，丢弃和错误均为 0，渲染吞吐为 29.788 FPS。
已知正脸图片序列路径完成 C++ RKNN 检测、关键点、PnP、30 帧校准和 Live2D
参数更新，最终健康状态为 `tracking_valid=true`、`calibrated=true`、置信度
0.922563；对应媒体路径再次写出 300 帧且无错误。RTSP 消费端读取结果为 H.264、
1920×1080、30 FPS。

同日又完成真实摄像头到浏览器的连续验收。摄像头真人画面完成 30/30 帧校准，
健康接口返回 `tracking_valid=true`、`calibrated=true`、置信度 0.904445；运行中
调用 `/recalibrate` 后，日志再次记录 30/30 校准完成。MediaMTX 建立 H.264 WebRTC
会话，浏览器视频元素持续播放 121.6 秒，分辨率为 1920×1080，3651 帧中丢弃 11
帧，媒体错误为空。`/view` 的 1.35 倍缩放和位置偏移也已通过浏览器截图确认。
本次追踪结果到 Godot 参数应用的分段延迟窗口平均约 7.7–12.4 ms，最大 36.9 ms；
该数值不包含摄像头曝光、编码、网络和浏览器解码，不能当作端到端延迟。取证完成后
用 `SIGTERM` 停止持续运行，退出码 143 来自本次主动清理。

历史构建记录不覆盖 ABI v3；本轮以 [模块边界](module-boundaries.md) 的验证范围为准，尚未在
RK3588 上运行。仍需完成新边界的板端回归、C++/Python 数值精度对照、长时间
稳定性和摄像头到浏览器的关联时间戳端到端延迟测量。

## Python 文件的定位

`tools/openseeface_3d_preview.py`、`tools/live2d_face_tracking.py` 和相关测试继续保留，
用于历史结果复核、模型转换和 C++/Python 数值对照。产品启动脚本、Godot 场景和
实时 Live2D 链路不再导入或启动这些 Python 文件。
