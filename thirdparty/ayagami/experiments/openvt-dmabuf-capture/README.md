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
- 单组 RGBA/NV12 DMA-BUF；
- `glFinish` 后同步调用 RGA 和 MPP；Godot 与外部 EGLImage 的纹理方向一致，GPU copy 不执行 Y 翻转；
- 使用 `/dev/dma_heap/system-uncached`；
- 仍保留 X11，只验证人物到硬件编码的共享缓冲链路；
- 不修改 Xorg、驱动、内核、设备树、权限或开机服务。

连续产品版本仍需三缓冲、显式 fence、背压和异常恢复。

## RK3588 实机结果

2026-10-27 使用阿芙洛狄忒模型完成 300 帧实机运行：

```text
1920×1080，30 FPS，300 帧，9.9999 秒
H.264 / yuv420p
核心链路吞吐 39.264 FPS
GPU copy 平均 1.244 ms
RGA 平均 2.109 ms
MPP 平均 3.956 ms
```

`ffprobe` 统计 300 帧，FFmpeg 完整解码无错误。6 个时间点抽帧哈希均不同，
视觉检查确认方向、颜色和人物动作正确。

本机结果：

```text
build/remote-artifacts/openvt-godot-dmabuf-20261027/run-03/
  openvt-ayagami-dmabuf-1920x1080-30fps.mp4
  godot-render.log
  ffprobe.txt
  decode.log
  preview-contact-sheet.jpg
```

最终 MP4 SHA256：

```text
29d43d436c0d084df16a356ccb8bc4890ca6c66a1e53180305266ee059bba7a4
```

