# C++ 人脸跟踪到 Live2D 同进程链路

## 当前结构

实时 Live2D 产品路径由 `vtb-live2d` 主程序统一编排。摄像头、RKNN、几何估计和
参数映射位于 LibGodot 外部；Godot 只接收已经生成的 Live2D 参数帧：

```text
OpenCV V4L2 camera source
  → capacity-one LatestFrameSlot
  → RKNN detector (NPU core 0)
  → RKNN 66-point landmark model (NPU core 1)
  → OpenSeeFace-compatible C++ PnP / expression features
  → C++ Live2DFaceMapper
  → FaceTrackingService capacity-one parameter snapshot
  → openvt_libgodot_application_submit_live2d_frame()
  → Godot main thread parameter input
  → Ayagami model parameters
  → SubViewport → DMA-BUF → RGA → MPP H.264
  → OutputManager → RTSP
```

Godot/Ayagami、渲染提交和媒体输出继续使用已经验证的实现。`vtb-live2d` 可执行文件
直接链接视频输入、OpenCV、RKNN 和 `FaceTrackingService`；LibGodot 共享库不再编译
摄像头、RKNN 模型、几何估计和人脸管线源码。

## 模块边界

- `OpenCvVideoSource`：打开摄像头或 OpenCV 支持的文件源，输出 BGR CPU 帧租约。
- `PipelineRuntime`：采集线程和推理线程；交接槽容量固定为一，推理落后时覆盖旧帧。
- `RknnFaceDetector` / `RknnLandmarkEstimator`：加载 `.rknn` 模型并固定到两个 NPU core。
- `OpenSeeFaceGeometryEstimator`：保留 70 点模板、14 点 PnP、Euler 角、66 点反投影和
  Remedian 表情特征语义。
- `Live2DFaceMapper`：中立姿态校准、限幅、参数映射和按单调时钟计算的一阶平滑。
- `FaceTrackingService`：纯 C++ 服务，接收独立创建的 `VideoSource` 和 `FacePipeline`，
  管理容量一的参数快照、校准和 HTTP 控制线程。
- `VtbApplication`：分别创建视频输入与模型管线，读取最新参数快照并提交给 Godot。
- `Live2DParameters`：独立的十参数值对象；Godot 只编译这个数据类型，不编译
  `Live2DFaceMapper`。
- `OpenVTLive2DFrameInput`：宿主与 LibGodot 之间的固定数值帧对象，只包含十个
  Live2D 参数、取景状态、结果时间戳和 generation；通过 C linkage 函数提交。
- `EmbeddedLive2DApplication`：Godot 内部渲染/输出编排，只消费参数帧并驱动 Ayagami。

推理回调不访问 Godot 对象。它只更新纯 C++ mailbox；宿主主线程通过固定桥接函数
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

映射结果默认使用 50 ms 时间常数。参数快照超过 150 ms 后，Godot 内部的
`EmbeddedLive2DApplication` 以 250 ms 时间常数把模型恢复到默认值。

## 启动

运行脚本启动一个 `vtb-live2d` 进程，Godot 作为进程内 LibGodot 实例运行：

```bash
OPENVT_CONFIG=/path/to/deployment/config/vtb-live2d.json \
./experiments/godot/embedded-runtime/run_on_rk.sh
```

配置结构见 [`video-input-configuration.md`](video-input-configuration.md) 和
[`config/vtb-live2d.example.json`](../config/vtb-live2d.example.json)。启动时先通过
V4L2 确认设备类型、采集能力和实际协商的 FourCC、尺寸、帧率，再由 OpenCV V4L2
后端读取。当前产品配置只接受 `v4l2_camera` 输入。

## 控制口

同一个 C++ 进程默认监听 `192.168.77.2:11575`：

- `GET /health`：返回校准、跟踪置信度、校准进度和取景状态。
- `POST /recalibrate`：请求推理线程在下一次更新前清空中立姿态。
- `POST /view`：更新 `zoom`、`offset_x` 和 `offset_y`。

绑定地址、端口和浏览器来源由 JSON 的 `control` 段提供。控制口没有认证或加密，
只适用于受信的直连本地网段。

## 构建依赖

主程序的 CMake 构建需要 OpenCV 与 RKNN sysroot：

```bash
export OPENVT_VTB_SOURCE_ROOT=/path/to/VTB
export OPENVT_BOARD_SYSROOT=/path/to/board-sysroot
export OPENVT_FFMPEG_SYSROOT=/path/to/ffmpeg-sysroot
export OPENVT_OPENCV_ROOT=/path/to/arm64-opencv-sysroot
export OPENVT_RKNN_ROOT=/path/to/arm64-rknn-sysroot
```

Godot 内置模块只需要 `OPENVT_VTB_SOURCE_ROOT`、`OPENVT_BOARD_SYSROOT` 和
`OPENVT_FFMPEG_SYSROOT`。OpenCV 与 `librknnrt.so` 由 `vtb-live2d` 宿主链接并在部署
目录提供运行库；它们不再成为 LibGodot 模块依赖。

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

本次宿主/LibGodot 边界拆分已经通过 ARM64 交叉编译和动态依赖检查，尚未在
RK3588 上运行。仍需完成新边界的板端回归、C++/Python 数值精度对照、长时间
稳定性和摄像头到浏览器的关联时间戳端到端延迟测量。

## Python 文件的定位

`tools/openseeface_3d_preview.py`、`tools/live2d_face_tracking.py` 和相关测试继续保留，
用于历史结果复核、模型转换和 C++/Python 数值对照。产品启动脚本、Godot 场景和
实时 Live2D 链路不再导入或启动这些 Python 文件。
