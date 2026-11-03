# 视频输入与人脸模型管线架构

## 1. 文档状态

本文描述 VTB 单进程 C++ 重构中的视频输入、硬件解码、图像预处理和人脸模型交互边界。当前内容是后续实现依据，不代表 RK3588 设备链路已经验证完成。

## 2. 设计目标

- 当前产品只支持 V4L2 摄像头输入。
- 模型不感知输入来源、压缩格式、V4L2、MPP 或 RGA。
- 摄像头、解码器、RGA 和 RKNN 之间保留 DMA-BUF 与零 CPU 整帧复制能力。
- 使用 RAII 管理帧的所有权，业务代码不手动调用释放接口。
- 摄像头采集与模型推理可以并行运行，视频采用容量为一的最新帧交接策略。
- 检测模型和关键点模型共享同一张完整解码帧，避免重复采集和重复解码。

## 3. 设计边界

本文中的“零拷贝”指避免 CPU 参与整帧像素搬运。MPP 解码和 RGA 裁剪、缩放、颜色转换仍然会读取源缓冲区并写入目标缓冲区。

统一输入接口本身不保证零拷贝。零拷贝成立还需要满足以下条件：

- V4L2、MPP、RGA 和 RKNN 使用可相互导入的 DMA-BUF 或等价硬件缓冲区。
- 下游使用完成前，缓冲区不能被生产者覆盖或复用。
- 异步硬件操作以完成回调或 fence 表示真正的读取完成。
- CPU 后备路径明确标记为 CPU 内存路径。

## 4. 组件关系

```text
+---------------------------------------------------------------------+
|                         vtb-live2d 进程                              |
|                                                                     |
|  VideoProcessingRuntime                                             |
|       +-- VideoSource                                               |
|       |       `-- CameraSource("/dev/video0")                        |
|       |                                                             |
|       `-- VideoFrameConsumer                                        |
|               `-- FaceFrameConsumer                                 |
|                       `-- FacePipeline                               |
|                               +-- ImageTransformer                   |
|                               +-- FaceDetector                       |
|                               `-- LandmarkEstimator                  |
+---------------------------------------------------------------------+
```

各组件职责：

| 组件 | 职责 |
| --- | --- |
| `VideoSource` | 从 V4L2 摄像头读取一帧并产出完整原始帧租约 |
| `DecodedFrameLease` | 持有完整解码帧及其生命周期 |
| `ImageTransformer` | 按模型要求完成 ROI 裁剪、缩放和颜色转换；RK3588 实现使用 RGA |
| `ModelInputSpec` | 描述模型所需宽高、像素格式、布局、数据类型和归一化约定 |
| `ModelInputLease` | 持有已经满足模型要求的输入缓冲区 |
| `FacePipeline` | 组织检测、跟踪、关键点和姿态计算 |
| `VideoFrameConsumer` | 统一同步消费一帧租约，不包含具体模型和结果类型 |
| `FaceFrameConsumer` | 把通用帧租约交给 `FacePipeline` 并发布人脸结果 |
| `LatestFrameSlot` | 在采集线程和模型线程之间保存最新一帧，替换时自动释放旧帧 |
| `VideoProcessingRuntime` | 组合视频源和消费者，管理线程、停止、latest-only 和错误传播 |

人脸模块的 `face_types.hpp` 保存检测、关键点、姿态、表情和跟踪结果值类型；
`face_pipeline.hpp`、`face_frame_consumer.hpp` 保存单帧算法流程和同步消费适配。
应用层 `apps/vtb-live2d/live2d_tracking_session.hpp` 组合视频运行时与 Live2D mapper，
`live2d_tracking_snapshot.hpp` 保存产品快照和取景状态，`live2d_control_server.hpp`
负责 HTTP 控制。`include/vtb/live2d/live2d_mapping_result.hpp` 独立保存映射结果；
值类型不包含算法、线程和 HTTP。完整依赖图见 [模块边界](module-boundaries.md)。

`VideoProcessingRuntime` 的启动、停止、等待和析构由拥有它的控制线程顺序调用。
采集线程和消费线程只访问内部状态对象；运行时在释放该对象前完成两个线程的
`join()`。内部状态仅为实际跨线程的停止请求、异常和计数保留同步。

## 5. 输入格式选择

```text
                          VideoSource
                               |
                               | read()
                               v
                 +----------------------------+
                 | 判断输入来源和实际格式     |
                 +----------------------------+
                     |          |
              Camera MJPG  Camera YUYV
                     |          |
                     v          |
                V4L2 DQBUF      |
                     |          |
                     v          |
                 MPP Decode     |
                     |          |
                     +----------+
                                |
                                v
                   DecodedFrameLease
             +--------------------------------+
             | format: NV12 / YUYV            |
             | memory: DMA-BUF / CPU fallback |
             | width/height/stride             |
             | timestamp/frame_id              |
             +--------------------------------+
```

格式对应关系：

| 输入格式 | 输入性质 | 目标路径 |
| --- | --- | --- |
| MJPG/MJPEG | 压缩帧 | V4L2 → MPP → 解码帧 |
| YUYV/YUY2 | 原始 YUV 4:2:2 | V4L2 → 解码帧视图，后续直接交给 RGA |
| NV12/NV21 | 原始 YUV | V4L2 → 解码帧视图，后续直接交给 RGA |

设置摄像头以后必须读取驱动回填的 FourCC、宽高、stride 和帧率。FourCC、宽高和
帧率当场与请求比较，不一致就终止启动；stride 作为帧内存布局保留。

## 6. 模型输入契约

模型只声明输入要求，不创建摄像头或 RGA 实例。

```text
ModelInputSpec
    width        = 224
    height       = 224
    pixel_format = RGB888（示例，需以模型元数据验证）
    layout       = NHWC（示例，需以 RKNN 输入属性验证）
    data_type    = UINT8（示例，需以 RKNN 输入属性验证）
    normalization= RKNN内部或外部，必须明确
```

模型输入准备过程：

```text
DecodedFrameLease
        |
        | FrameView + CropRegion + ModelInputSpec
        v
ImageTransformer::prepare()
        |
        | 产品路径: RGA
        | 对照路径: CPU
        v
ModelInputLease
        |
        v
FaceDetector / LandmarkEstimator
```

`ImageTransformer` 可以根据输入和输出规格选择最短路径。例如 YUYV 可以由一次 RGA 操作同时完成 ROI 裁剪、缩放和 RGB 转换。MPP 输出的 NV12 也直接交给 RGA，无需提前生成完整分辨率 RGB 中间帧。

当前产品路径使用 `RgaImageTransformer`。它直接导入 V4L2 帧的 DMA-BUF，并由一次
同步 RGA 操作完成 ROI 裁剪、缩放和 BT.601 limited YUV 到 RGB888 转换。输出缓冲区
从 JSON 指定的 DMA heap 分配并复用。输出同时保留 DMA-BUF fd 和 CPU 映射地址；
当前 `rknn_inputs_set()` 仍通过 CPU 地址接收数据，因此这一步还没有形成 RGA 到 RKNN
的完整零拷贝。后续只有在实际测量证明输入复制是瓶颈后，才需要改为 RKNN 内存导入。

两个 RKNN 模型都在 `RknnModel` 初始化时根据固定的输出属性预分配 FP32 缓冲区。
`rknn_outputs_get()` 使用 `is_prealloc = 1` 直接写入这些缓冲区，检测和关键点后处理
通过 `RknnOutputLease` 读取同一块内存；每帧不再创建输出 `vector<float>`，也不再
执行应用层 `memcpy`。租约释放前禁止下一次同模型推理覆盖缓冲区。该边界实现了
RKNN Runtime 输出到 CPU 后处理之间的应用层零拷贝；`want_float = 1` 仍可能让
RKNN Runtime 在内部把模型原生输出转换为 FP32，因此不等同于 NPU 输出内存的原生
零拷贝。若后续测量确认这段转换是瓶颈，需要改用 RKNN zero-copy IO memory API，
并让 CPU 后处理读取模型原生输出类型。

`CpuImageTransformer` 和 `RgaImageTransformer` 都实现同一个 `ImageTransformer`
接口。CPU 实现保留用于功能对照和不依赖 RGA 的构建；当前 `vtb-live2d` 组装时明确
选择 RGA 实现。RGA 失败时不会自动切换到 CPU，避免运行性能和内存路径发生未声明的
变化。RGA 路径解决 CPU 预处理开销和整帧 CPU 读取问题；它不会改变检测与关键点
模型的调用顺序，也不会自行降低 RKNN 推理耗时。验证指标应分别记录两种实现的
`prepare()` 耗时、模型输入差异、`rknn_inputs_set()` 到 `rknn_run()` 的耗时以及
完整消费帧耗时。

第一版预处理把源图 ROI 直接缩放到模型尺寸，不加入未记录的 letterbox 或 padding。
`ModelInputView` 保存本次预处理使用的源图 ROI。检测框、关键点和更新后的人脸框统一使用
源图像素坐标；模型后处理负责根据源 ROI 的偏移和缩放比例恢复坐标。

## 7. 人脸模型处理流

```text
DecodedFrameLease
        |
        | 需要重新检测
        v
RGA: full frame -> detector input
        |
        v
DetectorInputLease
        |
        v
FaceDetector
        |
        v
FaceBBox
        |
        v
RGA: FaceBBox ROI -> landmark input
        |
        v
LandmarkInputLease
        |
        v
LandmarkEstimator
        |
        v
66点关键点 + 置信度
        |
        v
FaceGeometry / PoseEstimator
        |
        v
FaceTrackingResult
        |
        +----------------------+----------------------+
        |                                             |
        v                                             v
Live2DFaceMapper                              Debug/Observability
        |                                             |
        v                                             v
Godot/Live2D                              延迟、丢帧和缓冲池统计
```

稳定跟踪时可以使用上一帧更新后的人脸框，跳过检测模型：

```text
上一帧人脸框
    |
    v
RGA: FaceBBox ROI -> landmark input
    |
    v
LandmarkEstimator
    |
    v
更新人脸框和关键点
```

重新检测的触发条件由 `FacePipeline` 管理，例如首次启动、人脸丢失、置信度下降或定期校正。

## 8. 缓冲区生命周期

### 8.1 V4L2 术语

```text
DQBUF：从摄像头驱动取得已经填充完成的缓冲区
QBUF ：把缓冲区归还驱动，允许摄像头再次写入
```

硬件消费者读取完成以前不能执行 `QBUF`。

### 8.2 MJPEG 摄像头

```text
CameraSource              MPP                 RGA               RKNN
     |                     |                   |                  |
     |-- V4L2 DQBUF ------>|                   |                  |
     |   MJPEG Buffer      |                   |                  |
     |                     |-- Decode          |                  |
     |                     |                   |                  |
     |<-- 输入读取完成 -----|                   |                  |
     |                     |                   |                  |
     |-- V4L2 QBUF         |                   |                  |
     |                     |                   |                  |
     |                     |-- NV12 DMA-BUF -->|                  |
     |                     |                   |-- Transform ---->|
     |                     |                   |   ModelInput     |
     |                     |                   |                  |
     |                     |                   |       RKNN推理   |
     |                     |                   |                  |
     |                     |                   |<-- 推理完成 -----|
     |                     |                   |                  |
     |                     |                   | 归还模型输入缓冲 |
```

MPP 读取完压缩输入以后，V4L2 缓冲区即可归还。完整解码帧继续由 `DecodedFrameLease` 持有，直到检测输入和关键点输入所需的 RGA 读取全部完成。

### 8.3 YUYV 摄像头

```text
CameraSource                 RGA                     RKNN
     |                        |                        |
     |-- V4L2 DQBUF -------->|                        |
     |   YUYV DMA-BUF        |                        |
     |                        |-- Crop/Scale/CSC ----->|
     |                        |                        |
     |<-- RGA读取完成 --------|                        |
     |                        |                        |
     |-- V4L2 QBUF            |                        |
     |                        |                        |
     |                        |   ModelInputLease      |
     |                        |----------------------->|
     |                        |                        |
     |                        |              RKNN推理  |
     |                        |                        |
     |                        |<------- 推理完成 ------|
     |                        |                        |
     |                        | 自动归还模型输入缓冲区 |
```

如果关键点 ROI 需要等待检测结果，YUYV 原始帧必须保持有效到第二次 RGA 读取完成。若这一持有时间导致 V4L2 缓冲区不足，可以增加应用自有的完整解码帧池，并通过一次硬件转换尽快归还摄像头缓冲区。是否引入该步骤由实际阻塞和延迟数据决定。

## 9. RAII 所有权

资源类使用只可移动的租约表达独占所有权，消费者不调用公开的 `release()`：

```cpp
class FrameLease {
public:
    ~FrameLease();

    FrameLease(const FrameLease&) = delete;
    FrameLease& operator=(const FrameLease&) = delete;

    FrameLease(FrameLease&& other) noexcept;
    FrameLease& operator=(FrameLease&& other) noexcept;

    const FrameView& view() const noexcept;
};
```

同步消费：

```cpp
FrameLease frame = source.acquire();
frame_consumer.consume(std::move(frame));
// consume 返回以后，frame 在消费者内部离开作用域并自动归还缓冲区。
```

异步交接：

```cpp
FrameLease frame = source.acquire();
latest_frame.publish(std::move(frame));
// 所有权已经移动到 LatestFrameSlot。
```

移动 `FrameLease` 只转移缓冲区句柄和元数据，不复制图像像素。异步硬件 API 返回“已提交”时仍需继续持有租约，直到硬件完成信号确认不再读取输入。

## 10. 线程交互

```text
CaptureThread
     |
     | source.read()
     v
DecodedFrameLease
     |
     | move
     v
LatestFrameSlot
     |
     | wait_and_take()
     v
ConsumerThread
     |
     | VideoFrameConsumer::consume()
     |
     `-- FaceFrameConsumer
             +-- RGA detector input
             +-- FaceDetector
             +-- RGA landmark input
             `-- LandmarkEstimator
     |
     v
FaceTrackingResult
     |
     v
RenderThread
     |
     +-- Live2D参数更新
     `-- Godot渲染
```

当新帧替换尚未被模型线程取得的旧帧时：

```text
NewFrameLease --> LatestFrameSlot --> OldFrameLease析构 --> 归还缓冲区
```

采集线程与模型线程异步交接；模型线程内部先采用同步 RGA 和同步推理语义。后续只有在测量证明需要时，才增加 fence 驱动的硬件异步并行。

## 11. 对外接口关系

```text
VideoSource
    |
    | read()
    v
DecodedFrameLease
    |
    | view()
    v
FrameView
    |
    | + CropRegion
    | + ModelInputSpec
    v
ImageTransformer
    |
    | prepare()
    v
ModelInputLease
    |
    | view()
    v
ModelInputView
    |
    v
FaceDetector / LandmarkEstimator
    |
    v
FaceTrackingResult
```

模型不得自行创建输入流。应用负责构造输入源并注入 `FacePipeline` 所需能力，从而允许同一模型用于摄像头、文件、单张图片和测试输入。

## 12. 简单方案与选择理由

简单方案是让模型直接接收 `cv::Mat`，并由每个模型自行裁剪和转换。该方案便于快速验证，但会把输入来源、CPU 内存布局和模型实现绑定在一起，也无法表达 DMA-BUF 生命周期。

本文选择 `DecodedFrameLease → ImageTransformer → ModelInputLease`，主要解决以下问题：

- 将输入来源与模型推理解耦。
- 明确跨线程和跨硬件组件的缓冲区所有权。
- 允许同一张完整解码帧供检测模型和关键点模型使用。
- 保留 RGA 直接写入 RKNN 输入缓冲区的优化空间。

该设计本身不会自动提高 FPS，也不会自动形成零拷贝。实际收益需要设备验证。

## 13. 验证指标

实现后至少记录以下指标：

- 摄像头请求值、驱动回填的 FourCC、宽高、stride 和 FPS，以及严格校验结果。
- 每帧经过的 DMA-BUF fd、缓冲池槽位和生命周期事件。
- CPU 整帧复制次数和复制字节数。
- V4L2 `DQBUF` 到 `QBUF` 的持有时间分布。
- MPP 解码、RGA 检测输入、检测推理、RGA 关键点输入、关键点推理的 P50/P95/P99。
- `LatestFrameSlot` 替换帧数量。
- V4L2 缓冲区耗尽、缓冲池等待和硬件 fence 等待次数。
- 整体从摄像头时间戳到 `FaceTrackingResult` 的 P50/P95/P99。
- 同一输入下 C++ 与当前 Python/OpenSeeFace 的检测框 IoU、66 点平均/P95/最大像素误差。
- C++ 与 Python 的 yaw/pitch/roll 差值、丢脸率、重新捕获帧数和重新检测触发时机。
- 重复启动、停止和异常退出后的 fd、mmap、DMA-BUF 与 RKNN context 泄漏情况。

只有在上述证据确认没有 CPU 整帧复制，并且相同 DMA-BUF 或明确的硬件目标缓冲区在组件间传递后，才把对应设备路径标记为零 CPU 拷贝。

## 14. 分阶段实现

1. 定义 `FrameView`、`DecodedFrameLease`、`ModelInputSpec` 和 `ModelInputLease` 的最小契约。
2. 实现摄像头 `VideoSource`，读取并严格校验驱动回填值。
3. 实现 RGA `ImageTransformer`，覆盖检测和关键点所需格式。
4. 接入 MPP 摄像头 MJPEG 解码。
5. 接入容量为一的 `LatestFrameSlot` 和模型线程。
6. 在 RK3588 上验证格式、缓冲区生命周期、复制次数和阶段延迟。
7. 测量确认需要后，再引入应用自有完整帧池或硬件异步 fence 管线。

## 15. 当前实现状态

当前分支已经完成架构的基础层：

- `FrameView`、`FramePlaneView` 和只可移动的 `DecodedFrameLease`。
- `ModelInputSpec`、`ModelInputView` 和只可移动的 `ModelInputLease`。
- V4L2 单平面 MMAP 采集、`DQBUF/QBUF` 生命周期和可用时的
  `VIDIOC_EXPBUF` DMA-BUF 导出。
- 容量为一的 `LatestFrameSlot`，替换旧帧时通过租约析构归还底层缓冲区。
- `FacePipeline` 对检测、关键点、姿态计算和重新检测策略的同步编排。
- `VideoFrameConsumer` 统一消费接口和 `FaceFrameConsumer` 人脸适配器。
- `VideoProcessingRuntime` 的采集线程、消费线程、latest-only 交接、停止顺序和
  异常保存；该运行时不依赖人脸类型。
- `RgaImageTransformer` 直接导入 V4L2 DMA-BUF，并在一次同步 RGA 操作中完成
  YUYV/NV12/NV21 ROI 裁剪、缩放和 224x224 RGB888 转换。
- `CpuImageTransformer` 保留相同的 `ImageTransformer` 契约，用于功能对照和
  不依赖 Rockchip RGA 的构建；产品入口不做隐式 CPU 回退。
- `RknnFaceDetector` 与 `RknnLandmarkEstimator`，分别封装 RKNN context、
  NPU core 选择、输入输出张量检查、推理资源释放以及 OpenSeeFace 检测框和
  66 点关键点后处理。
- `ConfigurationFileReader` 与 `VtbApplicationConfigParser` 分别负责读取配置文件和
  生成分段的强类型配置。
- `V4l2Camera` 在执行 `VIDIOC_S_FMT` 和 `VIDIOC_S_PARM` 后立即将驱动回填的
  FourCC、尺寸和帧率与请求值严格比较，不接受驱动回退，也不生成第二套配置对象。
- `CameraSource` 作为当前 Live2D 直接 V4L2 摄像头入口，使用 MMAP、
  `VIDIOC_DQBUF/QBUF` 和可用时的 `VIDIOC_EXPBUF`。
- `OpenSeeFaceGeometryEstimator` 的 14 点 PnP、Euler 角、66 点三维反投影和
  眼睛/嘴部自适应特征。
- `Live2DFaceMapper` 的中立姿态校准、十个 Ayagami 参数映射和按单调时钟平滑。
- 应用层 `Live2DTrackingSession` 组合视频运行时、算法和映射状态；独立的
  `Live2DControlServer` 只调用状态查询、取景修改和校准重置。
- `VtbApplication` 通过 ABI v2 提交参数快照；Godot 返回编码包视图，由宿主
  `MediaOutputSession` 拷贝并交给 `OutputManager`。协议配置位于 `output` 段。

MPP 摄像头 MJPEG 解码和 RGA 到 RKNN 的直接内存导入仍属于后续按测量引入的硬件
优化。当前 C++ Live2D 主路径直接采集 YUYV/NV12/NV21，由 RGA 生成模型输入；
`rknn_inputs_set()` 继续从 RGA 输出缓冲区的 CPU 映射地址接收数据，因此仍不能标记为
RGA 到 RKNN 端到端零拷贝。

- RKNN 和 RGA 分别通过 `VTB_ENABLE_RKNN_FACE`、`VTB_ENABLE_RGA_FACE` 启用；应用负责把输入、预处理、
  模型和后续姿态计算组合进同一进程。

当前 `CameraSource` 只接受请求为 YUYV、NV12 或 NV21 且驱动回填值完全一致的
完整原始帧。请求 MJPEG、H.264 或其他压缩格式时会明确报错，避免把压缩数据标记成
`DecodedFrameLease`。Live2D 应用根据 JSON 中显式声明的 `v4l2_camera` 类型创建
`CameraSource`。当前产品入口没有压缩摄像头解码、文件和网络读取实现。

以下设备实现仍待后续阶段完成：

- MJPEG 摄像头的 MPP 解码。
- RK3588 设备上的 DMA-BUF 缓冲区生命周期和摄像头到浏览器关联时间戳延迟验证。

当前基础层、RKNN 模型层、OpenCV PnP 几何层和 Live2D mapper 使用
Buildroot/Godot ARM64 GCC 13.2.0 构建。2026-10-30 的拆分前路径已在 RK3588 打开摄像头并
完成 300 帧 C++ 人脸运行时、Godot/Ayagami、RGA、MPP 和 RTSP 写出验证；另用
已知正脸图片序列确认 C++ RKNN、PnP、30 帧校准和参数映射进入有效状态。真实
摄像头真人画面也完成 30/30 校准，置信度为 0.904445；重新标定、视图控制以及
H.264 WebRTC 浏览器播放均已验证。浏览器连续播放 121.6 秒时共显示 3651 帧、
丢弃 11 帧且无媒体错误。当前仍未完成 C++/Python 数值精度对照、长时间稳定性和
带关联时间戳的摄像头到浏览器端到端延迟测量。此前宿主/LibGodot 边界拆分的编译记录不覆盖 2026-11-03 的 ABI v2；
本轮验证边界见 [模块边界](module-boundaries.md)，尚未进行 RK3588 板端回归。
