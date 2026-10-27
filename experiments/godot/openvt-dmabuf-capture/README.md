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
H.264 → MP4
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
- 一个常驻媒体线程按帧序执行 RGA、MPP 和 H.264 文件写入；
- 队列满时生产端阻塞形成背压；Godot 与外部 EGLImage 的纹理方向一致，GPU copy 不执行 Y 翻转；
- 使用 `/dev/dma_heap/system-uncached`；
- 仍保留 X11，只验证人物到硬件编码的共享缓冲链路；
- 不修改 Xorg、驱动、内核、设备树、权限或开机服务。

当前版本仍保留 `glFinish` 作为 GPU/RGA 的正确性边界。连续产品版本还需显式 fence、实时场景的最新帧丢弃策略、异常注入和长时间稳定性验证。

## RK3588 实机结果

2026-10-27 使用阿芙洛狄忒模型完成同步基线和两轮两阶段流水实机运行。三次运行使用相同的当前 Ayagami 项目、模型、动画输入和验证脚本：

| 配置 | 核心吞吐 | 脚本墙钟吞吐 | GPU copy | RGA | MPP |
|---|---:|---:|---:|---:|---:|
| 同步对照 | 44.924 FPS | 44.683 FPS | 1.129 ms | 2.003 ms | 3.952 ms |
| 流水第 1 轮 | 72.238 FPS | 71.523 FPS | 0.876 ms | 1.604 ms | 4.108 ms |
| 流水第 2 轮 | 78.990 FPS | 78.385 FPS | 0.746 ms | 1.356 ms | 3.971 ms |

两轮流水平均核心吞吐为 `75.614 FPS`，相对同机同步对照提高约 `68.3%`。两轮均无生产端背压，最大 ready queue 深度为 1。第二轮媒体线程平均耗时 `5.327 ms`，P95 为 `5.506 ms`；queue wait 平均 `0.014 ms`。

这里提升的是稳态处理吞吐：渲染下一帧时，媒体线程可以处理上一帧。单帧从参数更新到 H.264 可用的端到端延迟仍包含渲染、GPU copy、排队以及 RGA/MPP；不能把 `5.327 ms` 当作完整单帧延迟。对 30 FPS 输出而言，当前结果已经提供明显的处理余量。

三次输出均为 1920×1080、30 FPS、300 帧、9.9999 秒、H.264/yuv420p。`ffprobe` 统计 300 帧，FFmpeg 完整解码无错误。同步版和两轮流水版的 H.264、MP4 以及 6 个抽帧均逐字节相同；视觉检查确认方向、颜色、透明边缘和人物动作正确。

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

