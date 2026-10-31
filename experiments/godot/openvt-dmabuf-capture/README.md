# Godot/Ayagami → DMA-BUF → RGA → MPP 人物验证

这个实验保留现有 Godot 4.7.1 + Ayagami + X11 + Mali-G610 人物渲染路径，
并增加一个内置 Godot 模块。模块在 Godot render thread 内取得 SubViewport
的 GLES texture native handle，使用 `glBlitFramebuffer` 把人物画面复制到
外部分配的 RGBA DMA-BUF，再交给 RGA 转换 NV12 和 MPP 编码 H.264。

```text
Ayagami / Godot SubViewport GL texture
    ↓ same EGL context, GPU glBlitFramebuffer
RGBA DMA-BUF
    ↓ RGA
NV12 DMA-BUF
    ↓ MPP
H.264 packet
    ↓ OutputManager → RtspOutput
RTSP/TCP → MediaMTX
```

## 为什么先使用 GPU 内部复制

直接让 Godot 把 SubViewport 的颜色附件分配到外部 DMA-BUF，需要改动 Godot
render-target 的创建、resize、override 和销毁生命周期。当前阶段先使用同一
EGL Context 内的 GPU copy，可以去除 `get_image()`、`glReadPixels()`、CPU
原始像素缓冲和 CPU JPEG 编码，同时保持 Godot 默认 render target 逻辑不变。

该方案多一次 GPU 纹理复制，后续只有在测量证明它成为瓶颈时，才继续改为
Godot 直接渲染到外部颜色附件。

## 当前正确性版本

- 1920×1080、30 FPS、300 帧；
- 3 个 RGBA DMA-BUF FrameSlot，媒体线程独占 1 个 NV12 DMA-BUF；
- Godot render thread 完成 `glBlitFramebuffer` 和 `glFinish` 后把 FrameSlot 放入有界 FIFO；
- 一个常驻媒体线程按帧序执行 RGA、MPP 和 `OutputManager::publish()`；
- MPP 的 SPS/PPS 在输出启动前注册到 H.264 流描述，编码包携带单调时钟 PTS/DTS、帧序号和 IDR 标记；
- `RtspOutput` 在独立输出线程中通过 RTSP/TCP 发布，有界队列容量为 16；
- 队列满时生产端阻塞形成背压；Godot 与外部 EGLImage 的纹理方向一致，GPU copy 不执行 Y 翻转；
- 使用 `/dev/dma_heap/system-uncached`；
- 仍保留 X11，只验证人物到硬件编码的共享缓冲链路；
- 不修改 Xorg、驱动、内核、设备树、权限或开机服务。

实时推流由 `Engine.max_fps = OUTPUT_FPS` 把 Godot 提交节奏限制为 30 FPS。启动命令不使用
`--fixed-fps`，因为该选项会关闭实时同步并让离线帧以设备可达到的速度生成；在实时 RTSP
链路中会造成实际提交帧率、MPP 的 30 FPS 码控配置和浏览器解码能力不一致。验证以实际
提交帧率、RTSP 时间戳和浏览器丢帧数为准，不以固定 `delta` 代表实时帧率。

Live2D H.264 使用 MPP FIXQP、QP 18，以固定量化质量替代 CBR 的目标/最小/最大码率。
该模式不设置应用层码率边界：静态画面码率较低，大幅运动和面部特写会产生更高码率。
它用于优先验证源画质与运动画质，不解决人脸输入帧率不足，也不保证 100 Mbps 链路和
浏览器在最坏画面下不会拥塞。验证需要同时记录实际码率、浏览器丢帧和网络发送错误。

当前版本仍保留 `glFinish` 作为 GPU/RGA 的正确性边界。连续产品版本还需显式 fence、实时场景的最新帧丢弃策略、异常注入和长时间稳定性验证。

## 为什么直接接入 OutputManager

原先的最短替代方案是把 Annex-B 文件路径换成 FIFO，再启动独立 publisher 进程。
当前实现直接复用项目内的 `OutputManager + RtspOutput`，让 MPP 包的时间戳、关键帧、
重连和有界队列策略留在同一进程内，减少额外进程和 FFmpeg 再探测。该改造没有消除
编码包到 `MediaPacket`、再到 FFmpeg `AVPacket` 的内存复制，也不保证网络断连时零丢帧。
是否继续优化复制次数，以端到端 P95/P99、CPU、队列深度和丢帧数为依据。

内置模块构建需要提供 VTB 源码、Rockchip 板端 sysroot 和 FFmpeg sysroot：

```bash
export OPENVT_VTB_SOURCE_ROOT=/path/to/VTB
export OPENVT_BOARD_SYSROOT=/path/to/board-sysroot
export OPENVT_FFMPEG_SYSROOT=/path/to/ffmpeg-sysroot
```

该 Godot 模块只负责 Live2D 渲染帧的 DMA-BUF、RGA、MPP 和 RTSP 输出。OpenCV 与
RKNN 已移到 `vtb-live2d` 宿主，不进入 LibGodot 模块的源码和链接列表。

## 与实时人脸输入的边界

`run_render_on_rk.sh` 保留为 Godot-hosted 固定动画和媒体输出对照路径。实时人脸产品
路径使用 `experiments/godot/embedded-runtime/run_on_rk.sh` 启动 `vtb-live2d`：

```bash
OPENVT_CONFIG=/path/to/deployment/config/vtb-live2d.json \
./experiments/godot/embedded-runtime/run_on_rk.sh
```

视频输入、模型推理、校准和控制口由宿主中的纯 C++ `FaceTrackingService` 管理；
配置来自 JSON，启动前会验证 V4L2 设备和实际协商参数。
Godot 通过固定 C ABI 只接收最终 Live2D 参数帧。完整边界见
[`docs/live2d-face-tracking-bridge.md`](../../../docs/live2d-face-tracking-bridge.md)。

## RK3588 实机结果

2026-10-27 使用阿芙洛狄忒模型完成同步基线和两轮两阶段流水实机运行。三次运行使用相同的当前 Ayagami 项目、模型、动画输入和验证脚本：

| 配置 | 核心吞吐 | 脚本墙钟吞吐 | GPU copy | RGA | MPP |
|---|---:|---:|---:|---:|---:|
| 同步对照 | 44.924 FPS | 44.683 FPS | 1.129 ms | 2.003 ms | 3.952 ms |
| 流水第 1 轮 | 72.238 FPS | 71.523 FPS | 0.876 ms | 1.604 ms | 4.108 ms |
| 流水第 2 轮 | 78.990 FPS | 78.385 FPS | 0.746 ms | 1.356 ms | 3.971 ms |

两轮流水平均核心吞吐为 `75.614 FPS`，相对同机同步对照提高约 `68.3%`。两轮均无生产端背压，最大 ready queue 深度为 1。第二轮媒体线程平均耗时 `5.327 ms`，P95 为 `5.506 ms`；queue wait 平均 `0.014 ms`。

这里提升的是稳态处理吞吐：渲染下一帧时，媒体线程可以处理上一帧。单帧从参数更新到 H.264 可用的端到端延迟仍包含渲染、GPU copy、排队以及 RGA/MPP；不能把 `5.327 ms` 当作完整单帧延迟。对 30 FPS 输出而言，当前结果已经提供明显的处理余量。

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

