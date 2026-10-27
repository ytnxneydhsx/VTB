# RK3588 无桌面 Live2D UVC 视频流实施计划

## 1. 目标

最终设备在没有 HDMI、显示器、桌面环境和可见窗口的情况下完成：

```text
USB 真人摄像头
    ↓
V4L2 采集
    ↓
OpenSeeFace 或 RKNN 人脸参数
    ↓
Godot + Ayagami
    ↓
Mali-G610 Headless EGL 离屏渲染
    ↓
RGA 颜色转换
    ↓
MPP MJPEG 编码
    ↓
UVC Gadget
    ↓
电脑识别为普通 USB 摄像头
```

首个产品目标：

```text
1920×1080
稳定 30 FPS
MJPEG UVC 输出
无 Xorg/X11/Wayland
无可见窗口
无中间图片或视频文件
```

## 2. 为什么要做 Headless EGL

当前已经可工作的基线是：

```text
Xorg + armsoc + Rockchip libmali
    ↓
Godot X11/EGL
    ↓
Ayagami SubViewport
```

它证明了模型、Ayagami、定制 Godot 和 Mali-G610 可以工作，但引入：

- Xorg 进程；
- X11 Native Window；
- LightDM/Xfce 当前运行状态；
- DRI2 与窗口生命周期；
- 与最终纯后台 UVC 设备无关的显示故障面。

最终产品只需要 GPU Context 和离屏 Framebuffer。引入 Headless EGL 的目的属于系统精简、启动路径收敛和运行可靠性，不宣称它天然提升 Live2D 渲染 FPS。

## 3. 更简单的备选方案

### 3.1 最小 Xorg 基线

关闭 LightDM 和 Xfce，只运行最小 Xorg 与 1×1 Godot 宿主窗口：

```text
Xorg
  ↓
1×1 Native Window
  ↓
Godot SubViewport
  ↓
UVC
```

优点：

- 当前已经验证；
- 修改量小；
- 可以作为 Headless EGL 开发期间的回退路径。

代价：

- 仍然保留 Xorg/X11；
- 不满足最终完全无窗口系统的要求。

### 3.2 完全重写 Live2D 渲染器

用 C/C++ 和 OpenGL ES 直接加载 Cubism 模型：

```text
自定义 Live2D Runtime
    ↓
Headless EGL
    ↓
Mali-G610
```

优点是控制完整、运行时较小。代价是重新实现 `.moc3`、Drawable、Mask、混合模式、动作和参数系统，并重新确认 Cubism Core 的授权边界。

当前不选择该方案，因为 Ayagami 和模型兼容性已经验证，重写成本明显高于给 Godot 增加 Headless EGL 后端。

## 4. 当前 Godot 的限制

已检查实际使用的 Godot 4.7.1 源码：

- `--headless` 等价于 Headless DisplayServer + Dummy Renderer；
- 官方 Headless 模式会禁用真实图形渲染；
- Linux X11 EGL 后端调用 `eglCreateWindowSurface()`；
- 当前没有 Linux Headless EGL Pbuffer/Surfaceless DisplayServer；
- Ayagami 依赖真实 RenderingServer，不能使用 Dummy Renderer。

所以简单增加 `--headless` 参数不能解决问题，需要修改 Godot Linux 显示/EGL 初始化层。

## 5. 所需驱动和用户态组件

| 环节 | 内核驱动 | 用户态组件 | 是否必需 |
|---|---|---|---|
| USB 摄像头输入 | `uvcvideo`、V4L2 | V4L2 API | 有真人输入时必需 |
| NPU 人脸推理 | `rknpu` | `librknnrt.so` | 使用 RKNN 时必需 |
| Live2D GPU 渲染 | `bifrost_kbase` | `libmali`、EGL、OpenGL ES | 必需 |
| 图像转换 | Rockchip RGA | `librga.so` | 推荐 |
| MJPEG/H.264 编码 | Rockchip MPP/VCodec | `librockchip_mpp.so` | 压缩输出时必需 |
| USB 摄像头输出 | DWC3 UDC、UVC Gadget | ConfigFS、V4L2 Output | 必需 |

Xorg、X11、LightDM、Xfce 和 HDMI 不在最终必需清单中。

## 6. 分阶段实施

### 阶段 0：冻结当前可工作的基线

目的：Headless EGL 开发失败时能够回到已经验证的 Mali-G610 路径。

保留：

```text
定制 Godot ARM64 二进制
armsoc_drv.so
20-armsoc.conf
CPU/GPU 基准日志
1920×1080 离屏截图
远端构建容器和命令记录
```

完成标准：

- 所有产物 SHA256 可复核；
- Xorg 路径仍可重复得到 Mali-G610；
- 现有模型仍可输出 1920×1080 图片。

当前状态：已完成。

### 阶段 1：独立验证无 Xorg EGL 能力

目的：在改 Godot 前确认当前 Rockchip `libmali` 实际支持哪一种无窗口 EGL 模式。

创建最小 ARM64 C/C++ 探针，按以下顺序测试：

1. EGL Surfaceless Context；
2. EGL Pbuffer；
3. EGL GBM/DRM。

探针只做：

```text
加载 libmali
查询 EGL 版本和扩展
创建 OpenGL ES Context
创建 64×64 FBO
绘制固定颜色
glReadPixels
校验像素
退出
```

验证时必须区分：

- 没有 `DISPLAY`；
- Xorg/LightDM 已停止；
- Renderer 是 Mali-G610；
- 不允许回退到 llvmpipe；
- 探针退出后 GPU 恢复空闲；
- SSH 和系统保持稳定。

风险控制：

- 每种后端单独运行并设置超时；
- 先验证 Surfaceless/Pbuffer，再尝试 GBM；
- 尝试 GBM 前重新确认当前 5.10 BSP 和 `libmali` 的稳定性；
- 停止 LightDM 属于运行状态变更，执行前再次确认 SSH 回滚路径；
- 不同时修改内核、设备树或 libmali 版本。

阶段完成标准：至少一种无 Xorg EGL 模式可以稳定创建 Mali-G610 Context，并正确读回 64×64 图像。

### 阶段 2：实现 Godot Headless EGL DisplayServer

目的：保留 Godot RenderingServer 和 Ayagami，同时移除 Native Window 依赖。

计划新增：

```text
DisplayServerHeadlessEGL
Headless EGL/GL Manager
LinuxBSD 构建开关
opengl3_es 注册和选择逻辑
```

核心行为：

```text
窗口、鼠标、键盘接口返回空实现
EGL Context 使用阶段 1 验证成功的后端
RenderingServer 保持启用
SubViewport、CanvasItem、Mesh、Texture 和 Shader 保持可用
Ayagami blend mode 补丁继续保留
```

阶段完成标准：

- `DISPLAY` 未设置；
- 系统没有 Xorg/Wayland 进程；
- Godot 输出 Mali-G610；
- Ayagami 模型成功加载；
- 1920×1080 `SubViewport` 输出视觉正确；
- 600 帧测试达到至少 30 FPS；
- 连续运行 30 分钟无崩溃、GPU hang 或内存持续增长。

### 阶段 2.5：验证 GPU、RGA、MPP 的 DMA-BUF 共享链路

目的：在修改 Godot 的 `SubViewport` 资源生命周期前，先证明当前 BSP 的
Mali、RGA 和 MPP 能够围绕同一组 DMA-BUF 正确协作。

当前基线链路：

```text
SubViewport GPU Texture
    ↓ get_image / glReadPixels
CPU RGBA Image
    ↓ save_jpg / libjpeg-turbo
JPEG 文件
```

拟验证链路：

```text
DMA-BUF RGBA
    ↓ EGLImage / GL_TEXTURE_2D / FBO
Mali-G610 渲染
    ↓ fd 共享，不读取到 CPU
RGA RGBA → NV12
    ↓ NV12 DMA-BUF fd
MPP 硬件编码
    ↓
本地码流文件
```

先做独立 ARM64 探针，暂不修改 Godot。探针保留当前已验证的 X11 EGL
Context，只验证外部 DMA-BUF 渲染目标、RGA 导入和 MPP 导入。这样可以把
“共享缓冲区是否可行”和“移除 Xorg 是否可行”拆成两个独立问题。

采用该设计的原因：

- 当前逐帧导出约 14 FPS，纯渲染约 56 FPS；主要额外开销位于
  `get_image()`、RGBA/RGB 转换和 CPU JPEG 编码；
- MPP 和 RGA 都提供 DMA-BUF fd 导入接口；
- 在接口兼容性尚未验证前直接修改 Godot，失败时难以区分 EGL、格式、
  modifier、同步、RGA 或 MPP 中的具体问题。

更简单的备选方案仍然保留：

```text
SubViewport get_image
    ↓ CPU RGBA
RGA / MPP
```

它可以先去掉 CPU JPEG 编码，但仍保留完整画面读回和至少一次 CPU 到硬件
缓冲区传输。该方案作为功能回退，不作为零 CPU 像素搬运的完成标准。

该阶段不解决：

- Godot `SubViewport` 还不能指定外部颜色附件；
- Headless EGL 尚未实现；
- RGBA 与 NV12 是两个不同格式，RGA 仍需读取一块 DMA-BUF 并写入另一块；
- 编码码流仍需由 CPU 取出并写文件，但码流远小于 1080p 原始帧；
- 单缓冲区不能同时由 GPU、RGA 和 MPP 写读，连续链路需要至少三组缓冲区。

独立探针完成标准：

- 使用与当前 Godot 相同的 Rockchip `libmali`，日志确认 Mali-G610；
- 外部分配的 RGBA DMA-BUF 成功导入 EGLImage，并成为完整的 GLES FBO；
- GPU 绘制结果不经 `glReadPixels()`，由 RGA 直接读取同一 RGBA DMA-BUF；
- RGA 输出 NV12 DMA-BUF，MPP 直接导入该 fd；
- MPP 输出可独立解码的 1920×1080 本地码流；
- 日志记录每个 DMA-BUF fd、stride、format、modifier、各阶段耗时和错误码；
- 只有上述条件全部满足，才称为“GPU → RGA → MPP 无 CPU 像素复制”。

2026-10-27 验证结果：该独立探针已经通过。当前 BSP 的 CMA 总量只有 8 MiB
且空闲为 0，1080p RGBA 单帧需要 8,294,400 字节，因此连续 CMA heap 分配
失败；改用 `/dev/dma_heap/system-uncached` 后，Mali-G610 成功渲染到外部
RGBA DMA-BUF，RGA 直接转为 NV12 DMA-BUF，MPP 直接导入并编码。300 帧输出
封装为 1920×1080、30 fps、约 10 秒的 H.264 MP4，FFmpeg 完整解码。
同步单缓冲探针测得约 126 FPS，平均 GPU 1.66 ms、RGA 2.53 ms、MPP 3.74 ms。
该数字只代表纯色测试图和硬件交接，不代表 Ayagami 模型渲染性能。

同日再次运行真实 Ayagami 人物基线，阿芙洛狄忒模型成功输出 1920×1080、
30 FPS、300 帧、10 秒视频，抽帧确认眨眼、转头和摇头动作，FFmpeg 完整
解码。该人物基线仍通过 `SubViewport.get_texture().get_image()` 读回并保存
JPEG，实测捕获吞吐约 14.26 FPS。它验证人物内容和动作，尚未连接上述
DMA-BUF 探针；因此后续 Godot 外部颜色附件或同一 GPU Context 内的 GPU
拷贝桥接仍是必需工作。

同日已完成 GPU copy 桥接的真实人物链路：Godot render thread 取得实际
SubViewport GLES 纹理句柄，用 `glBlitFramebuffer` 写入 RGBA DMA-BUF，RGA
转换到 NV12 DMA-BUF，MPP 直接导入并编码。该路径没有调用 `get_image()`、
`glReadPixels()` 或 CPU 原始像素复制。300 帧输出为 1920×1080、30 FPS、
9.9999 秒 H.264 MP4，FFmpeg 完整解码，视觉检查方向和颜色正确。同步单缓冲
正确性版本测得约 39.26 FPS，平均 GPU copy 1.244 ms、RGA 2.109 ms、MPP
3.956 ms。该结果完成了本地文件第一版的 Godot → DMA-BUF → RGA → MPP
链路；多缓冲、显式 fence 和长时间运行仍属于阶段 3 的产品化工作。

进入 Godot 修改前的否决条件：

- `EGL_EXT_image_dma_buf_import` 缺失；
- DMA-BUF 可以导入但不能作为 `GL_COLOR_ATTACHMENT0`；
- RGA 或 MPP 不接受实际 stride、format 或 modifier；
- 只能通过 CPU 映射和复制才能得到正确图像；
- 同步只能靠未定义的隐式行为，无法稳定重复验证。

### 阶段 3：实现连续帧导出

目的：把 `SubViewport` 从“保存一张 PNG”改为持续生产帧缓冲区。

回退路径：

```text
SubViewport Texture
    ↓ get_image/readback
CPU RGBA Buffer Ring
```

首选路径在阶段 2.5 探针通过后改为：

```text
SubViewport GL Texture
    ↓ 同一 EGL Context 内 GPU copy（当前已验证）
RGBA DMA-BUF Ring
    ↓ RGA
NV12 DMA-BUF Ring
    ↓ MPP
Encoded Packet Ring
```

后续测量若证明这次 GPU copy 成为瓶颈，再把 `SubViewport GL Texture` 和
GPU copy 替换为 `SubViewport External Color Attachment`。

使用至少三组缓冲区：

```text
Buffer A：GPU 正在渲染
Buffer B：RGA/MPP 正在处理
Buffer C：UVC 正在发送
```

该阶段不写 PNG，不写视频文件。

阶段完成标准：

- 连续产生 1920×1080 RGBA 帧；
- 时间戳使用单调时钟；
- 生产者不会覆盖仍在消费的缓冲区；
- 10 分钟内无丢失所有权、越界和内存增长；
- DMA-BUF 路径记录 GPU→RGA 交接 P50/P95/P99 延迟；
- 回退路径记录 GPU→CPU 读回 P50/P95/P99 延迟。

### 阶段 4：接入 RGA

目的：把 Godot RGBA 输出转换为编码器或 UVC 需要的格式。

第一目标：

```text
RGBA 1920×1080
    ↓ RGA
NV12 1920×1080
```

诊断目标：

```text
RGBA
    ↓ RGA
YUYV
```

阶段完成标准：

- 颜色、透明背景合成和方向正确；
- RGA 转换达到稳定 30 FPS；
- 记录转换耗时和 CPU 占用；
- 与 CPU 参考转换抽样比对。

### 阶段 5：接入 MPP MJPEG 编码

目的：降低 USB 带宽并提高主机兼容性。

主路径：

```text
NV12
  ↓ MPP
MJPEG
```

H.264 作为后续可选格式，不作为第一版 USB 摄像头的完成条件。

阶段完成标准：

- 1920×1080 30 FPS MJPEG；
- 编码帧能够被独立解码；
- 帧顺序和时间戳正确；
- 连续 30 分钟无 MPP 错误或缓冲区泄漏；
- 记录码率、单帧大小、编码 P95 延迟。

### 阶段 6：接入 UVC Gadget

目的：让外部电脑把 RK3588 识别为标准 USB 摄像头。

UVC 描述符第一版只声明已经实际支持的格式：

```text
1920×1080
30 FPS
MJPEG
```

必要时增加低分辨率诊断模式，例如 1280×720 MJPEG。

阶段完成标准：

- macOS、Linux 至少各一台主机能够枚举摄像头；
- 能在系统摄像头应用中持续预览；
- 画面方向、颜色和帧率正确；
- USB 断开重连后服务能够恢复；
- 连续 1 小时无永久卡帧和缓冲区死锁。

### 阶段 7：接入 OpenSeeFace 参数

目的：使用真人输入实时驱动 Live2D。

先保留 CPU 参考路径：

```text
USB 摄像头
    ↓ V4L2
OpenSeeFace CPU
    ↓ 参数
Godot/Ayagami
```

在 CPU 结果正确并有性能数据后，再决定是否把模型转换为 RKNN：

```text
OpenSeeFace 模型
    ↓ RKNN Toolkit2
.rknn
    ↓ librknnrt
NPU
```

阶段完成标准：

- 头部、眼睛和嘴巴参数映射正确；
- 参数时间戳与输出视频时间戳可追踪；
- 端到端动作延迟 P95 达到确定目标；
- CPU 和 RKNN 结果抽样对照在可接受误差内。

### 阶段 8：零拷贝优化

只有在阶段 3 到阶段 7 的测量证明 GPU→CPU 读回或内存复制是瓶颈时，才引入：

```text
GPU Texture
    ↓ EGLImage / DMA-BUF
RGA
    ↓ DMA-BUF
MPP
    ↓
UVC
```

该阶段可能需要给 Godot 增加底层纹理句柄导出接口，或者通过 RenderingServer 的可用 Native Handle 接口实现。不会在缺少基准数据时提前修改这一层。

阶段完成标准：

- 明确减少的拷贝次数；
- 端到端延迟或 CPU 占用有可重复改善；
- 缓冲区所有权、同步 Fence 和生命周期有文档；
- 与非零拷贝路径输出一致。

## 7. 模块边界

### 7.1 Godot 核心修改

只承载：

- Ayagami 所需混合模式；
- Headless EGL DisplayServer；
- 必要时的 Native Texture/EGLImage 导出能力。

### 7.2 VTB C++ GDExtension

承载：

- OpenSeeFace/RKNN 参数输入；
- 参数到 Ayagami 的映射；
- 帧导出和循环缓冲区；
- RGA 调用；
- MPP 调用；
- UVC V4L2 输出；
- 指标和错误状态。

新增拥有 fd、映射、线程、锁或缓冲区的 C++ `class` 时，五个特殊成员函数必须明确实现、`= default` 或 `= delete`。独占资源禁止复制；移动操作只有在所有权和内部线程关系可安全转移时实现。

### 7.3 系统配置

承载：

- UVC Gadget ConfigFS；
- systemd 服务；
- 设备权限；
- 启动顺序；
- USB 断开重连恢复。

## 8. 性能指标

第一版完成门槛：

```text
输出：1920×1080 MJPEG 30 FPS
运行时间：至少 1 小时
永久卡帧：0
UVC 断开重连：可恢复
内存：无持续增长
GPU hang：0
```

必须分别记录：

- OpenSeeFace/RKNN 推理耗时；
- Live2D 参数更新耗时；
- Godot 渲染耗时；
- GPU→CPU 读回耗时；
- RGA 转换耗时；
- MPP 编码耗时；
- UVC 排队和发送耗时；
- 端到端动作延迟；
- CPU、GPU、NPU 利用率；
- RSS、DMA-BUF 和 CMA 使用量；
- 丢帧和缓冲区积压数量。

## 9. 明确不在当前阶段做的事情

- 不升级到主线内核或 Panthor；
- 不更换当前 Rockchip BSP；
- 不修改设备树频率表；
- 不同时升级 RKNN、MPP、RGA 和 GPU 栈；
- 不在缓冲区所有权、同步和背压完成前声称产品级零拷贝；
- 不提前支持多种 UVC 编码格式；
- 不用桌面窗口作为最终输出路径。

## 10. 下一步

当前保留 X11 的本地文件第一版已经完成。移除 X11 的下一项任务是阶段 1：

```text
编写最小 ARM64 Headless EGL 探针
在没有 DISPLAY、没有 Xorg 的条件下
验证 Surfaceless、Pbuffer 或 GBM 是否能驱动 Mali-G610
```

只有这个探针成功后，才开始修改 Godot。这样可以避免在底层 `libmali` 不支持目标模式时，先投入大量 Godot 源码改造。
