# 视频输入 JSON 配置与设备验证

## 设计目的

产品入口不再根据字符串猜测输入来源，也不在视频读取类中读取环境变量。配置文件
读取、JSON 解析、V4L2 设备验证和逐帧读取分别由独立模块负责：

```text
--config path
  → ConfigurationFileReader
  → VtbApplicationConfigParser
  → VideoInputFactory
      → CameraSource
          → V4l2Camera
          → V4L2 MMAP / DQBUF / QBUF
  → VideoProcessingRuntime
      → LatestFrameSlot
      → VideoFrameConsumer
          → FaceFrameConsumer
              → FacePipeline
```

配置读取、JSON 解析、V4L2 设备配置和缓冲区读取分属独立模块。当前
产品输入直接使用 Linux V4L2，视频读取路径不依赖 OpenCV `VideoCapture`。
以后增加 `network` 配置段或独立网络输入实现时，只需扩展
`VideoInputFactory` 和新的 `VideoSource` 实现。

`VideoProcessingRuntime` 只依赖统一的 `VideoSource` 和 `VideoFrameConsumer`。
人脸模块通过 `FaceFrameConsumer` 适配到消费接口，因此摄像头采集、latest-only
线程调度和异常传播不依赖 `FacePipeline` 或 `FaceTrackingResult`。

## 配置段

完整示例见 [`config/vtb-live2d.example.json`](../config/vtb-live2d.example.json)。
当前必须提供以下六段：

- `video_input`：输入类型、设备、FourCC、分辨率、帧率和缓冲区数量。
- `face_pipeline`：RKNN 模型路径、RGA DMA heap 路径和跟踪策略。
- `mapping`：校准、平滑和角度方向。
- `control`：HTTP 控制口。
- `render`：Godot 工程、Live2D 模型、输出画面尺寸、帧率和帧数上限。
- `output`：宿主输出协议配置，当前为 `rtsp_url`。

2026-11-03 起，将原 `render.rtsp_url` 移到 `output.rtsp_url`；保留旧字段会收到明确的
迁移错误。其他字段保持原义，板端配置需在下一次授权部署时同步迁移。

`VideoInputFactory` 位于 `apps/vtb-live2d`，由应用把 `VideoInputConfiguration` 转为
后端的 `CaptureConfig`。`vtb_capture` 和 `vtb_video_core` 不依赖应用配置库。

`video_input.type` 当前只接受 `v4l2_camera`。产品工厂只创建直接 V4L2
的 `CameraSource`。直接 V4L2 路径当前接受
`YUYV`、`NV12` 和 `NV21` 原始帧。`MJPG`、H.264 等压缩格式需要先接入解码器，
配置解析器会在启动时明确拒绝这些格式，避免把压缩字节当成像素帧。

`face_pipeline.rga_dma_heap` 指定 RGA 模型输入缓冲区使用的 DMA heap，例如
`/dev/dma_heap/system-uncached`。路径由 JSON 提供；`RgaImageTransformer` 不在代码中
猜测或固定设备节点。启动时无法打开该 heap、摄像头缓冲区无法导出 DMA-BUF，或
RGA 不支持请求的格式、stride、ROI 与缩放比例时，模型管线会明确报错并停止。

## 摄像头验证

启动时 `VideoInputFactory` 会按 `type` 选择输入实现。`CameraSource` 使用
同一次设备打开和格式设置完成严格验证：

1. `VIDIOC_QUERYCAP` 确认设备支持单平面视频采集和 streaming I/O。
2. `VIDIOC_S_FMT` 后读取实际 FourCC、宽度和高度。
3. `VIDIOC_S_PARM` 后读取实际帧间隔。
4. 实际结果与 JSON 请求逐项比较；驱动发生回退时启动失败并报告请求值和实际值。
5. `CameraSource` 直接申请 V4L2 MMAP 缓冲区，执行 `VIDIOC_QUERYBUF`、
   `VIDIOC_QBUF`、`VIDIOC_STREAMON` 和逐帧 `VIDIOC_DQBUF`。驱动通过
   `VIDIOC_REQBUFS` 返回的缓冲区数量必须与 JSON 的 `buffer_count` 完全一致。
6. 驱动支持 `VIDIOC_EXPBUF` 时导出 DMA-BUF fd；帧租约释放时再执行
   `VIDIOC_QBUF` 归还对应缓冲区。

JSON 中的 `CaptureConfig` 是产品配置契约。`CameraSource` 不保存或对外暴露
第二套配置；`V4l2Camera` 在每次 `ioctl` 后立即将驱动回填的 FourCC、尺寸和帧间隔
与请求值做严格比较。项目中没有驱动结果配置类。内部缓冲池保留 JSON 请求值；另保留
驱动给出的 `bytes_per_line`，用于正确跨过每行可能存在的对齐填充。它只是帧内存
布局信息，不会改变请求的格式、宽度、高度或帧率。

缓冲区是否位于驱动队列，以 V4L2 的 `DQBUF/QBUF` 状态为唯一依据。程序不再维护
重复的 `in_use` 影子数组；不可复制的 `DecodedFrameLease` 保证每次成功取出的
缓冲区只执行一次归还回调。不同线程释放帧时，`requeue_mutex_` 只负责串行化
`VIDIOC_QBUF`，不会重复记录驱动已经维护的 Buffer 所有权。

验证成功后才创建模型管线和 LibGodot。该流程会在运行设备上打开摄像头并设置格式，
因此交叉编译只能证明代码可构建；设备支持情况必须在获得授权后于 RK3588 上启动验证。

## 启动

复制示例并按部署路径修改：

```bash
cp config/vtb-live2d.example.json /path/to/deployment/config/vtb-live2d.json
```

直接运行：

```bash
vtb-live2d --config /path/to/deployment/config/vtb-live2d.json
```

板端脚本默认读取 `$OPENVT_EMBEDDED_DEPLOY_DIR/config/vtb-live2d.json`，也可以只用
`OPENVT_CONFIG` 覆盖配置文件位置。具体业务参数统一写入 JSON。
