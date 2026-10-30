# C++ 人脸跟踪到 Live2D 同进程链路

## 当前结构

实时 Live2D 产品路径由一个定制 Godot 进程承载。Python、JSON 和 UDP 已从该
运行路径移除：

```text
OpenCV V4L2 camera source
  → capacity-one LatestFrameSlot
  → RKNN detector (NPU core 0)
  → RKNN 66-point landmark model (NPU core 1)
  → OpenSeeFace-compatible C++ PnP / expression features
  → C++ Live2DFaceMapper
  → capacity-one parameter snapshot
  → Godot main thread
  → Ayagami model parameters
  → SubViewport → DMA-BUF → RGA → MPP H.264
  → OutputManager → RTSP
```

Godot/Ayagami、渲染提交和媒体输出继续使用已经验证的实现。人脸输入、推理、
几何、表情、校准、参数映射和浏览器控制口都编入同一个 Godot ARM64 可执行文件。

## 模块边界

- `OpenCvVideoSource`：打开摄像头或 OpenCV 支持的文件源，输出 BGR CPU 帧租约。
- `PipelineRuntime`：采集线程和推理线程；交接槽容量固定为一，推理落后时覆盖旧帧。
- `RknnFaceDetector` / `RknnLandmarkEstimator`：加载 `.rknn` 模型并固定到两个 NPU core。
- `OpenSeeFaceGeometryEstimator`：保留 70 点模板、14 点 PnP、Euler 角、66 点反投影和
  Remedian 表情特征语义。
- `Live2DFaceMapper`：中立姿态校准、限幅、参数映射和按单调时钟计算的一阶平滑。
- `OpenVTFaceTrackingRuntime`：Godot 内置类，负责上述对象的生命周期、容量一的
  参数快照以及 HTTP 控制线程。
- `render_dmabuf_video.gd`：只在 Godot 主线程取最新快照并写 Ayagami 参数。

推理回调不访问 Godot 对象。它只更新纯 C++ mailbox；`Dictionary` 在 Godot 主线程
调用 `take_latest()` 时构造。

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

映射结果默认使用 50 ms 时间常数。参数快照超过 150 ms 后，GDScript 以 250 ms
时间常数把模型恢复到默认值。

## 启动

运行脚本只启动一个 Godot 进程：

```bash
OPENVT_FACE_SOURCE=/dev/video0 \
OPENVT_FACE_WIDTH=1280 \
OPENVT_FACE_HEIGHT=720 \
OPENVT_FACE_FPS=60 \
OPENVT_FACE_BUFFERS=4 \
OPENVT_FACE_DETECTOR_MODEL=/home/huangnv/VTB/models/openseeface/mnv3_detection_fp16.rknn \
OPENVT_FACE_LANDMARK_MODEL=/home/huangnv/VTB/models/openseeface/lm_model3_fp16.rknn \
OPENVT_LIVE2D_TOTAL_FRAMES=0 \
./experiments/godot/openvt-dmabuf-capture/run_render_on_rk.sh
```

摄像头输入默认请求 MJPEG。OpenCV 构建启用了 V4L2、JPEG/PNG 和图像序列；固定
FFmpeg sysroot 当前缺少 `libswscale`，因此这个构建不承诺压缩视频文件的 FFmpeg
解码能力。

## 控制口

同一个 C++ 进程默认监听 `192.168.77.2:11575`：

- `GET /health`：返回校准、跟踪置信度、校准进度和取景状态。
- `POST /recalibrate`：请求推理线程在下一次更新前清空中立姿态。
- `POST /view`：更新 `zoom`、`offset_x` 和 `offset_y`。

默认只为 `http://192.168.77.1:8900` 返回浏览器跨域许可。可用
`OPENVT_LIVE2D_CONTROL_ADDRESS`、`OPENVT_LIVE2D_CONTROL_PORT` 和
`OPENVT_LIVE2D_CONTROL_ORIGIN` 覆盖。控制口没有认证或加密，只适用于受信的直连
本地网段。

## 构建依赖

Godot 内置模块除原有 Rockchip/FFmpeg sysroot 外，还需要：

```bash
export OPENVT_VTB_SOURCE_ROOT=/path/to/VTB
export OPENVT_BOARD_SYSROOT=/path/to/board-sysroot
export OPENVT_FFMPEG_SYSROOT=/path/to/ffmpeg-sysroot
export OPENVT_OPENCV_ROOT=/path/to/arm64-opencv-sysroot
export OPENVT_RKNN_ROOT=/path/to/arm64-rknn-sysroot
```

`SCsub` 从 ARM64 OpenCV `opencv5.pc` 读取共享库链接参数，并链接板端匹配的
`librknnrt.so`。Godot 与 OpenCV 都内建 JPEG/PNG 实现，因此最终 Godot 使用
OpenCV shared 构建隔离两组图像编解码符号；运行脚本通过
`OPENVT_OPENCV_LIB_DIR` 指定部署后的 OpenCV `.so` 目录。构建成功只证明 ARM64
编译和链接边界；摄像头、NPU、渲染、MPP、RTSP 和浏览器端到端仍需在 RK3588
上分别验收。

## Python 文件的定位

`tools/openseeface_3d_preview.py`、`tools/live2d_face_tracking.py` 和相关测试继续保留，
用于历史结果复核、模型转换和 C++/Python 数值对照。产品启动脚本、Godot 场景和
实时 Live2D 链路不再导入或启动这些 Python 文件。
