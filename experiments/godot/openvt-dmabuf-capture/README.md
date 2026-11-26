# Godot/Ayagami → RGBA DMA-BUF 人物输出

模块把四块带 DMA-BUF 的 GLES 纹理轮流设为 SubViewport 的颜色附件。
Godot 渲染前翻转 2D canvas，使纹理内存行序与编码端一致。GPU 完成写入后，
宿主把当前 RGBA 租约绑到 `GstMemory`，H.264 和 MJPEG 编码分支共用该输入。

```text
Godot SubViewport → 四槽 RGBA DMA-BUF → RgbaFrameLease → appsrc → tee
    ├─ mpph264enc → RTSP / WebRTC
    └─ mppjpegenc → UVC
```

`render_target_set_override()` 逐帧选择空闲纹理。Headless EGL 补丁 0007 修复
多纹理轮换时 GLES3 帧缓冲对象缓存和退出清理的问题。四个槽位都在用时跳过当前节拍，
最后一个 `GstMemory` 引用释放后槽位才可复用。停机时渲染线程负责释放 GL 和 EGL 资源，
底层 DMA-BUF 由仍在途的租约继续保活。宿主和 LibGodot 使用 ABI v8。

## 历史测量

实时推流由 `Engine.max_fps = OUTPUT_FPS` 把 Godot 提交节奏限制为 30 FPS。启动命令不使用
`--fixed-fps`，因为该选项会关闭实时同步并让离线帧以设备可达到的速度生成；在实时 RTSP
链路中会造成实际提交帧率、MPP 的 30 FPS 码控配置和浏览器解码能力不一致。验证以实际
提交帧率、RTSP 时间戳和浏览器丢帧数为准，不以固定 `delta` 代表实时帧率。

Live2D H.264 使用 MPP FIXQP、QP 18，以固定量化质量替代 CBR 的目标/最小/最大码率。
该模式不设置应用层码率边界：静态画面码率较低，大幅运动和面部特写会产生更高码率。
它用于优先验证源画质与运动画质，不解决人脸输入帧率不足，也不保证 100 Mbps 链路和
浏览器在最坏画面下不会拥塞。验证需要同时记录实际码率、浏览器丢帧和网络发送错误。

当前版本仍保留 `glFinish` 作为 GPU 写入完成边界。连续产品版本还需显式 fence、实时场景的最新帧丢弃策略、异常注入和长时间稳定性验证。

## 为什么让 GStreamer 管理双输出

当前宿主把原始 NV12 DMA-BUF 交给进程内 GStreamer。GStreamer 负责分支、线程、背压、
RTSP 发布和 UVC 协商，项目只在 appsink 回调里复用已验证的 MPP 编码资源类。编码包复制
到 GStreamer 自有缓冲后即可释放 MPP packet；未压缩像素不做 CPU 复制。

内置模块构建需要 VTB 源码与 Rockchip 板端 sysroot：

```bash
export OPENVT_VTB_SOURCE_ROOT=/path/to/VTB
export OPENVT_BOARD_SYSROOT=/path/to/board-sysroot
```

该 Godot 模块负责 Live2D 渲染帧的 RGBA DMA-BUF 与 GPU 完成同步；RGA、MPP、RTSP、
OutputManager、指标报告和 FFmpeg 位于宿主。同步编码回调后宿主拥有独立编码包副本，本次保留
一次编码字节复制。
V4L2/RKNN 以及仅用于 PnP 几何计算的 OpenCV 位于 `vtb-live2d` 宿主，不进入
LibGodot 模块的源码和链接列表。

## 与实时人脸输入的边界

`run_render_on_rk.sh` 为历史 Godot-hosted 对照入口，须搭配 ABI v2 之前的完整二进制。
当前模块移除了 GDScript 的 RTSP configure 入口，旧场景会明确提示迁移。实时人脸产品
路径使用 `experiments/godot/embedded-runtime/run_on_rk.sh` 启动 `vtb-live2d`：

```bash
OPENVT_CONFIG=/path/to/deployment/config/vtb-live2d.json \
./experiments/godot/embedded-runtime/run_on_rk.sh
```

宿主 `VtbApplication` 直接装配视频输入、两个消息通道和两个 Stage；
`Live2DControlServer` 通过 `FaceTrackingStage` 管理校准和控制口；
配置来自 JSON，启动时会验证 V4L2 设备，并要求驱动回填参数与请求完全一致。
Godot 通过固定 C ABI 只接收最终 Live2D 参数帧。完整边界见
[`docs/live2d-face-tracking-bridge.md`](../../../docs/live2d-face-tracking-bridge.md)。

## RK3588 历史流水线实机结果

2026-10-27 使用阿芙洛狄忒模型完成同步基线和两轮两阶段流水实机运行。三次运行使用相同的
Ayagami 项目、模型、动画输入和验证脚本：

| 配置 | 核心吞吐 | 脚本墙钟吞吐 | GPU copy | RGA | MPP |
|---|---:|---:|---:|---:|---:|
| 同步对照 | 44.924 FPS | 44.683 FPS | 1.129 ms | 2.003 ms | 3.952 ms |
| 流水第 1 轮 | 72.238 FPS | 71.523 FPS | 0.876 ms | 1.604 ms | 4.108 ms |
| 流水第 2 轮 | 78.990 FPS | 78.385 FPS | 0.746 ms | 1.356 ms | 3.971 ms |

两轮流水平均核心吞吐为 `75.614 FPS`，相对同机同步对照提高约 `68.3%`。两轮均无生产端背压，最大 ready queue 深度为 1。第二轮媒体线程平均耗时 `5.327 ms`，P95 为 `5.506 ms`；queue wait 平均 `0.014 ms`。

这里提升的是稳态处理吞吐：渲染下一帧时，媒体线程可以处理上一帧。单帧从参数更新到 H.264 可用的端到端延迟仍包含渲染、GPU copy、排队以及 RGA/MPP；不能把 `5.327 ms` 当作完整单帧延迟。对 30 FPS 输出而言，当前结果已经提供明显的处理余量。

2026-11-14 当前产品源码移除 READY 帧队列、三槽背压和内部媒体流水线，并把 RGA、MPP
分别移到宿主。2026-11-15 又将 Face 与 Godot 的 RGA 业务缓存分成两个 Session，并通过
共享双 Worker Executor 执行命令；每个业务生产者仍同步等待自己的单个在途请求，MPP 继续
由 Godot 所有者线程顺序调用。上表继续作为历史对照；
历史 ABI v7 直接渲染实现已通过 ARM64 编译。ABI v8 的 RK3588 验证还需测量吞吐、
P95/P99、画面正确性和退出资源释放。

以下结果来自改为 RTSP 之前的文件输出基线。三次输出均为 1920×1080、30 FPS、300 帧、9.9999 秒、H.264/yuv420p。`ffprobe` 统计 300 帧，FFmpeg 完整解码无错误。同步版和两轮流水版的 H.264、MP4 以及 6 个抽帧均逐字节相同；视觉检查确认方向、颜色、透明边缘和人物动作正确。新的 RTSP 路径仍需在 RK3588 上单独验证。

本机结果：

```text
build/remote-artifacts/openvt-godot-pipeline-20261027-01/
  build/
  run-logs/
  run-pipeline-v1-preview/preview-0120.png
```

最终 MP4 SHA256：

```text
29d43d436c0d084df16a356ccb8bc4890ca6c66a1e53180305266ee059bba7a4
```

新 Godot ARM64 产物 SHA256：

```text
3046122ec9ec09d65c1aad066345511ea66191ea02089147152ac892b83f513a
```

RK 上同步旧版和流水新版使用不同目录，运行时可通过 `OPENVT_GODOT_BIN` 选择，不需要覆盖旧产物。

