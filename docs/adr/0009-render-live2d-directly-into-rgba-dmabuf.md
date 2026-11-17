# ADR 0009：让 Godot 直接渲染到 RGBA DMA-BUF

- 状态：已实现，ARM64 编译通过，RK3588 运行验证待完成
- 日期：2026-11-17

## 背景

原链路先让 Godot 渲染到 SubViewport 自有颜色纹理，再用
`glBlitFramebuffer()` 复制到外部分配的 RGBA DMA-BUF。宿主等待 `glFinish()` 后，
把同一 DMA-BUF 同步交给 RGA 转换为 NV12。该实现边界清楚，但每帧多提交一次
1920×1080 RGBA 纹理复制。

## 决策

`OpenVTDmaBufCapture` 在初始化阶段完成以下长期资源装配：

1. 从 `/dev/dma_heap/system-uncached` 分配单个 RGBA DMA-BUF；
2. 把 DMA-BUF 导入为 `EGLImageKHR` 和 GLES2D texture；
3. 用 Godot 的 `texture_create_from_native_handle()` 包装该 texture；
4. 把包装后的纹理 RID 设为 Live2D SubViewport RenderTarget 的颜色附件。

此后 Godot iteration 直接把最终颜色写入该 DMA-BUF。逐帧路径仅保留
`glFinish()` 完成边界、同步 `RgbaFrameSink` 交付和 RGA RGBA→NV12 转换。
关闭时先解除 RenderTarget override，再释放 Godot 纹理 RID，最后销毁 GL texture、
EGLImage 和 DMA-BUF fd。

桥接 ABI 升级为 v7，移除逐帧 `acquire_viewport_texture()`、
`blit_viewport_texture()` 和 `wait_gpu_copy()`，新增 `wait_gpu_render()`。

## 选择理由

Godot 4.7.1 的 GLES3 后端已经支持 native texture 包装和 RenderTarget override，
内置模块可以复用现有接口，无需维护 Godot 核心补丁。该方案删除每帧 GPU blit，
同时保留现有单槽、同步借用和 RGA 同步读取协议，改动集中在 LibGodot 输出边界。

## 放弃的基线方案

保留 SubViewport 自有颜色纹理并继续执行 GPU blit，具有较低的 RenderTarget 生命周期
耦合，但会持续消耗一次全帧 RGBA 复制和对应命令提交。增加多槽异步流水也能隐藏部分
等待时间，却会扩大缓冲数量、fence 协议和借用生命周期；本阶段没有引入。

## 影响与限制

- RGBA 输入仍为单槽，宿主必须在下一次 Godot iteration 前同步完成 RGA 读取。
- `glFinish()` 仍是 GPU 与 RGA 之间的完成边界；显式 native fence 属于后续优化。
- RGA 输出池、GStreamer 队列、MPP 编码和线程结构保持不变。
- 目标 DMA-BUF 的尺寸在 LibGodot 实例生命周期内固定；动态 resize 未纳入本次实现。

## 验证

远端 ARM64 构建需要同时生成 ABI v7 LibGodot 与 `vtb-live2d` 宿主，并检查两者为
AArch64 ELF。RK3588 验收需记录画面方向、颜色与透明边缘、连续运行稳定性，以及
`live2d.godot_iteration`、`live2d.gpu_render_wait`、`live2d.rga_transform` 的
平均值、P95 和 P99。历史 `live2d.gpu_blit` 指标只作为旧实现对照。
