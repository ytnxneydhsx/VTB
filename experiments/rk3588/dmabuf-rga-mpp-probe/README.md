# RK3588 GPU → RGA → MPP DMA-BUF 探针

这个独立探针验证当前 RK3588 BSP 能否在不把原始像素读回 CPU 的条件下完成：

```text
DMA heap RGBA buffer
    ↓ EGL_EXT_image_dma_buf_import
Mali-G610 GLES FBO render
    ↓ 同一个 DMA-BUF fd
RGA RGBA → NV12
    ↓ NV12 DMA-BUF fd
MPP H.264 encode
```

探针保留已经验证的 X11 EGL Context，用于把 DMA-BUF 共享问题与 Headless EGL
问题分开验证。它不修改 Godot、Xorg、LightDM、驱动、内核、设备树或设备权限。

## 当前边界

- GPU 与 RGA 之间使用同一个 RGBA DMA-BUF；
- RGA 与 MPP 之间使用同一个 NV12 DMA-BUF；
- RGBA 与 NV12 是两种格式，因此仍然需要两个硬件缓冲区；
- 探针使用单缓冲区和同步 `glFinish`/RGA/MPP 调用，只验证正确性；
- 连续产品链路需要三组以上缓冲区和显式 fence；
- 编码码流由 CPU 写入文件，原始 1080p 像素不映射到 CPU；
- 当前设备节点是 root-only，运行脚本使用已经存在的无交互 `sudo`，不会修改权限。
- 当前板卡的 CMA 总量只有 8 MiB 且空闲为 0，无法容纳一张 1080p RGBA
  缓冲区；默认改用可提供散列表 DMA-BUF 的 `system-uncached` heap。可通过
  `DMA_HEAP_PATH` 覆盖，以便后续在其他 BSP 上对照测试连续内存 heap。

## 构建要求

项目规则要求在 `10.191.32.121` 远端服务器交叉编译 ARM64。构建必须使用：

- 当前 RK 板上复制的 MPP/RGA 头文件和动态库；
- 与既有 Godot 构建一致的 ARM64 Buildroot SDK；
- 任务专用远端目录；
- 构建 manifest、真实命令、退出码和 SHA-256。

CMake变量：

```text
BOARD_SYSROOT     当前RK的头文件和MPP/RGA库快照
KHRONOS_HEADERS   EGL/GLES Khronos头文件根目录
```

## 完成标准

- 日志中的GL renderer为Mali-G610；
- EGL DMA-BUF导入成功且FBO完整；
- RGA同步RGBA→NV12成功；
- MPP直接导入NV12 DMA-BUF并输出300帧H.264；
- 码流无重编码封装为30 fps MP4，`ffprobe`确认1920×1080、300帧、约10秒，
  FFmpeg可以完整解码；
- 每阶段记录平均耗时和整体吞吐率。

## 2026-10-27 实测结果

当前 RK3588 已通过完整探针：

- EGL 1.4，`GL_RENDERER=Mali-G610`；
- 300 帧 1920×1080 图案全部完成 GPU → RGA → MPP；
- 输出为 H.264/yuv420p，封装后为 30 fps、300 帧、约 10 秒 MP4；
- FFmpeg 完整解码，错误日志为空；
- 单缓冲同步探针约 126 FPS；平均 GPU 1.66 ms、RGA 2.53 ms、MPP 3.74 ms。

这个吞吐率用于证明共享缓冲区链路有充足余量，不代表 Ayagami 模型的实际
渲染帧率。当前尚未修改 Godot，也尚未实现多缓冲和显式 fence。
