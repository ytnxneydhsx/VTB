# ADR 0002：产品 Runtime 使用 Headless EGL

状态：接受，等待当前 LibGodot 产品链路实机验收
日期：2026-11-03

## 背景

ADR 0001 完成 LibGodot 生命周期迁移时保留了已经验证的 X11/EGL 路径。当前产品只把
Godot 用作离屏 Live2D 渲染器，不需要桌面窗口、输入法、剪贴板或窗口事件循环。运行
采样还观察到 Godot 主线程等待 X11 socket，因此产品继续依赖 Xorg 会增加无关的
启动条件和故障面。

## 决策

Godot 4.7.1 LinuxBSD 增加 `headless_egl` display driver。VTB 的 LibGodot 宿主默认
使用该驱动，通过 `/dev/dri/renderD128` 创建 GBM device、EGL Pbuffer 和 OpenGL ES
3 context。发布构建关闭 X11、Wayland 和 Vulkan，板端启动环境不再设置 `DISPLAY`、
`XAUTHORITY` 或 `MALI_DEFAULT_WINSYS=x11`。

本阶段保留 `SubViewport GL Texture -> glBlitFramebuffer() -> RGBA DMA-BUF`，从而
只改变显示后端，不同时改变渲染输出内存的所有权和槽位轮换。外部 RenderTarget 与
直接 DMA-BUF 渲染作为后续独立决策和验证阶段。

## 原因

最小 Xorg 加 1x1 窗口仍可作为回退基线，但它继续要求 X server 和 X11 native
window。Headless EGL 保留 Godot RenderingServer、Ayagami 和当前捕获模块，只替换
产品不需要的窗口系统边界，能够用较小的同时变更范围验证 X11 等待是否消失。

## 代价和边界

- 维护一组固定在 Godot `4.7.1-stable` 提交上的平台补丁；
- 依赖目标 BSP 匹配的 GBM/Mali 用户态库和 `/dev/dri/renderD128` 权限；
- BSP 当前 `x11-gbm` Mali 包可能仍间接携带 X11 客户端库，需通过进程 maps 单独核验；
- 本决策不承诺提升 Live2D FPS，也不删除 `glBlitFramebuffer()`；
- X11 构建产物保留为开发期回退，不进入默认产品启动路径。

## 验证

- ARM64 LibGodot 使用 `x11=no wayland=no vulkan=no opengl3=yes` 构建；
- `readelf -d` 无直接 X11/XCB/Wayland 依赖，二进制包含 `headless_egl`；
- 未设置 `DISPLAY` 时完成启动、渲染、RGA、MPP 和 RTSP 输出；
- 单路基线通过后再验证双路输出和 60 FPS 配置；
- 至少运行 10 分钟，submitted/encoded/written 无丢失，视觉结果正常；
- 正常退出和 `SIGTERM` 后线程、端口、DRM fd、DMA-BUF 与编码资源完整释放；
- 对比 X11 基线确认主线程 X11 socket 等待消失，并记录帧时间和 CPU/GPU 指标。
