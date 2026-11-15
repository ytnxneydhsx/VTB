# C++ 人脸跟踪到 Live2D 同进程链路

## 当前结构

实时 Live2D 产品路径由 `vtb-live2d` 主程序统一编排。摄像头、RKNN、几何估计和
参数映射位于 LibGodot 外部；Godot 只接收已经生成的 Live2D 参数帧：

```text
V4L2 CameraSource (MMAP / DQBUF / QBUF)
  → VideoCaptureStage
  → capacity-one BoundedFifoChannel<DecodedFrameLease>
  → FaceTrackingStage
  → RgaModelInputTransformer → Face RgaTransformSession
  → shared RgaExecutor (2 workers)
  → RKNN detector (NPU core 0)
  → RKNN 66-point landmark model (NPU core 1)
  → OpenSeeFace-compatible C++ PnP / expression features
  → C++ Live2DFaceMapper
  → capacity-one BoundedFifoChannel<Live2DTrackingSnapshot>
  → openvt_libgodot_application_submit_live2d_frame()
  → Godot main thread parameter input
  → Ayagami model parameters
  → SubViewport → RGBA DMA-BUF
  → RgbaFrameSink → host RgaNv12FrameTransformer → Godot RgaTransformSession
  → shared RgaExecutor (2 workers) → NV12
  → Nv12FrameSink
  → MediaEncodeSession
      ├─ MPP H.264 → EncodedVideoSink → OutputManager → RTSP
      └─ MPP MJPEG → EncodedVideoSink → OutputManager → GStreamer uvcsink → UVC
```

Godot/Ayagami、Viewport 纹理访问和 GPU copy 位于 LibGodot/OpenVT；RGA、MPP 与输出协议
位于宿主。`vtb-live2d` 可执行文件直接链接 V4L2 视频输入、OpenCV PnP 几何计算、RKNN、
Rockchip RGA/MPP 和三个流水线 Stage；LibGodot 共享库不编译摄像头、RKNN 模型、几何估计、
人脸管线、RGA、MPP、指标报告、OutputManager 或 RTSP 源码，也不链接 RGA/MPP/FFmpeg。

## 模块边界

- `CameraSource`：直接打开 V4L2 摄像头，管理 MMAP/DMA-BUF 缓冲区和
  `DQBUF/QBUF` 帧租约，当前输出 YUYV/NV12/NV21 原始帧。
- `VideoCaptureStage`：独占 `VideoSource` 和采集线程，把帧租约发布到容量一通道。
- `Live2DTrackingPipeline`：构造时固定 `FacePipeline` 和 `Live2DFaceMapper` 配置，
  同步完成单帧人脸处理与映射，不创建线程或持有消息通道。
- `FaceTrackingStage`：只连接已配置任务主体、输入通道和输出通道；在自己的线程中
  阻塞取得帧租约、调用主体并发布快照。
- `VtbApplication`：直接拥有三个 Stage、两条 `BoundedFifoChannel` 和共享
  `RgaExecutor`，统一安排启动、停止和错误传播。旧 `VideoProcessingRuntime` 只保留给
  独立协议测试。
- `RgaExecutor`：容量四的 FIFO 和两个 Worker；每个 Worker 独占一个
  `RgaTransformer/RgaDevice`，不绑定 RGA 核心。
- `RgaModelInputTransformer`：FacePipeline 直接持有的模型输入适配器；通过 Face 私有
  Session 管理摄像头源注册缓存和 RGB888 目标池。
- `RknnFaceDetector` / `RknnLandmarkEstimator`：加载 `.rknn` 模型并固定到两个 NPU core。
- `OpenSeeFaceGeometryEstimator`：保留 70 点模板、14 点 PnP、Euler 角、66 点反投影和
  Remedian 表情特征语义。
- `Live2DFaceMapper`：中立姿态校准、限幅、参数映射和按单调时钟计算的一阶平滑。
- 跟踪快照通道：容量一并采用 `drop_oldest`；Godot 所有者线程使用 `try_pop()`
  非阻塞取得新快照。
- `Live2DControlServer`：独立的 HTTP 控制组件，借用寿命更长的 `FaceTrackingStage`，
  序列化状态和转发命令。
- `MediaOutputSession`：宿主编码包接收器，按 H.264/MJPEG 类型复制回调中的借用字节，
  管理 RTSP/UVC 两个输出的启动、排空和停止。
- `MediaEncodeSession`：同步借用 Godot/OpenVT 交付的 NV12 DMA-BUF，独占 H.264/MJPEG
  两个 MPP 会话，并把编码包发布给 `MediaOutputSession`；当前不创建线程或帧队列。
- `RgaNv12FrameTransformer`：同步借用 LibGodot 交付的 RGBA DMA-BUF，通过 Godot 私有
  Session 向共享 Executor 提交 RGBA→NV12 命令，等待完成后把 NV12 Lease 借给
  `MediaEncodeSession`。
- `VtbApplication`：分别创建视频输入、人脸跟踪管线与 Godot 渲染管线，在组合根直接实例化通道和 Stage，
  管理顶层启动、停止和错误传播。
- `GodotRenderPipeline`：同步渲染任务主体；`create()` 只装配配置与依赖，随后在调用它的
  Godot 所有者线程内创建、驱动和销毁 LibGodot，不持有消息通道。
- `GodotRenderStage`：独占 `GodotRenderPipeline` 和 Godot 所有者线程，绑定跟踪快照通道，
  负责非阻塞消费、帧调度、停止、join 与异常传播。
- `Live2DParameters`：独立的十参数值对象；Godot 只编译这个数据类型，不编译
  `Live2DFaceMapper`。
- `OpenVTLive2DFrameInput`：宿主与 LibGodot 之间的固定数值帧对象，只包含十个
  Live2D 参数、取景状态、结果产生时间戳和 snapshot_generation；通过 C linkage 函数提交。
- `EmbeddedLive2DApplication`：Godot 内部渲染编排，只消费参数帧并驱动 Ayagami、Viewport
  纹理取得、GPU blit、GPU wait 和 RGBA 交付。

FaceTrackingStage 不访问 Godot 对象。它调用已配置的 `Live2DTrackingPipeline` 取得映射结果，
再把值快照发布到纯 C++ Channel；
Godot 所有者线程通过固定桥接函数
提交数值字段，跨边界不再传递 Godot `Dictionary`、摄像头对象或模型对象。

## 参数映射

启动后的前 30 个有效帧用于计算中立头姿中位数：

| C++ 跟踪结果 | Live2D 参数 | 规则 |
|---|---|---|
| yaw - neutral yaw | `ParamAngleX` | 乘以 `angle_x_sign`；当前同向配置为 +1，再限幅 ±30° |
| pitch - neutral pitch | `ParamAngleY` | 默认限幅 ±30° |
| roll - neutral roll | `ParamAngleZ` | 默认限幅 ±30° |
| 三个头部角度 | `ParamBodyAngleX/Y/Z` | 乘以 0.3 |
| 左右眼自适应特征 | `ParamEyeLOpen/ROpen` | 限幅 0～1 |
| `mouth_open` | `ParamMouthOpenY` | 归一化到 0～1 |
| `mouth_wide` | `ParamMouthForm` | 限幅 -1～1 |

Godot 所有者线程直接调用 `tracking_channel_.try_pop()` 非阻塞读取。取得快照时
先 `submit()` 再执行本周期 `run_frame()`；通道暂无快照时继续使用最近参数。通道
已 `finished()` 且队列已排空时直接退出渲染循环，该周期不执行 `run_frame()`。
`snapshot_generation` 随映射或取景变化递增，来源帧号保留在映射结果的
`frame_id()`。取景更新保持原映射时间戳。

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

Godot 内置模块只需要 `OPENVT_VTB_SOURCE_ROOT`、`OPENVT_BOARD_SYSROOT` 提供公共项目头文件
和 Linux DMA-BUF/DRM 头文件。OpenCV、RKNN、RGA、MPP 和 FFmpeg 由宿主链接。产品构建启用 `VTB_ENABLE_EMBEDDED_GODOT`、
`VTB_ENABLE_OPENCV_FACE_PIPELINE`、`VTB_ENABLE_RKNN_FACE`、`VTB_ENABLE_RGA_FACE`、
`VTB_ENABLE_RTSP_OUTPUT`。两个推理后端关闭时，通用会话与消费适配仍可独立编译。

ABI v6 使用 `openvt_libgodot_application_initialize_v6()`、版本查询和配置尺寸校验，
只把 `RgbaFrameSink` 和输出几何传入 LibGodot。渲染分段计时和 JSONL 报告器由宿主
`GodotRenderPipeline` 持有。
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

历史构建记录不覆盖 ABI v6；本轮以 [模块边界](module-boundaries.md) 的验证范围为准，尚未在
RK3588 上运行。仍需完成新边界的板端回归、C++/Python 数值精度对照、长时间
稳定性和摄像头到浏览器的关联时间戳端到端延迟测量。

## Python 文件的定位

`support/tools/openseeface_3d_preview.py`、`support/tools/live2d_face_tracking.py` 和相关测试继续保留，
用于历史结果复核、模型转换和 C++/Python 数值对照。产品启动脚本、Godot 场景和
实时 Live2D 链路不再导入或启动这些 Python 文件。
