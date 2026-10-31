# 视频输入 JSON 配置与设备验证

## 设计目的

产品入口不再根据字符串猜测输入来源，也不在视频读取类中读取环境变量。配置文件
读取、JSON 解析、V4L2 设备验证和逐帧读取分别由独立模块负责：

```text
--config path
  → ConfigurationFileReader
  → VtbApplicationConfigParser
  → VideoInputFactory
      → V4l2DeviceValidator
      → OpenCvVideoSource
  → PipelineRuntime
```

直接把 JSON 解析和设备探测写进 `OpenCvVideoSource` 可以减少类的数量，但会让视频
模块同时依赖文件系统、JSON 格式和摄像头协议。当前拆分让以后增加 `network` 配置段
或独立网络输入实现时，无需修改 V4L2 摄像头读取逻辑。它改善的是配置正确性和模块
边界，不会自动提高帧率或降低延迟。

## 配置段

完整示例见 [`config/vtb-live2d.example.json`](../config/vtb-live2d.example.json)。
当前必须提供以下五段：

- `video_input`：输入类型、设备、FourCC、分辨率、帧率和缓冲区数量。
- `face_pipeline`：RKNN 模型路径和跟踪策略。
- `mapping`：校准、平滑和角度方向。
- `control`：HTTP 控制口。
- `render`：Godot 工程、Live2D 模型、RTSP 地址和输出规格。

`video_input.type` 当前只接受 `v4l2_camera`。`VideoInputFactory` 是输入实现的
选择边界。后续网络输入应增加新的类型和对应 `VideoSource` 实现；网络
连接、重连和认证参数放入独立配置段，不进入 `OpenCvVideoSourceConfig`。

## 摄像头验证

启动时 `VideoInputFactory` 会按 `type` 选择输入实现。V4L2 路径会先交给
`V4l2DeviceValidator` 打开配置中的设备并执行能力协商：

1. `VIDIOC_QUERYCAP` 确认设备支持单平面视频采集和 streaming I/O。
2. `VIDIOC_S_FMT` 后读取实际 FourCC、宽度和高度。
3. `VIDIOC_S_PARM` 后读取实际帧间隔。
4. 实际结果与 JSON 请求逐项比较；驱动发生回退时启动失败并报告请求值和实际值。
5. OpenCV V4L2 后端再次设置并读取 FourCC、尺寸、帧率和缓冲区数量，确认读取层
   接受同一配置。

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
