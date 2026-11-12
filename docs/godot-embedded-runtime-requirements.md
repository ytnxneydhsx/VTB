# VTB 嵌入式 Godot Runtime 需求说明

状态：第一版实现完成，实机验收记录见 `godot-embedded-runtime-validation-2026-10-30.md`
日期：2026-10-30；2026-11-03 增补当前模块边界。

历史实机验收不覆盖 ABI v3。当前实现的模块职责与验证范围以
[模块边界](module-boundaries.md) 为准，下文保留尚未接入产品的接口设计。
2026-11-03 的 Headless EGL 后续阶段由
[ADR 0002](adr/0002-use-headless-egl-for-product-runtime.md) 管理；下文关于 X11 的
第一版范围描述保留为历史边界。
适用目标：RK3588 ARM64 单进程 Live2D 产品链路

## 1. 文档目的

本文定义 VTB 将 Godot 改造成可由应用层 C++ 主动初始化、逐帧驱动和关闭的嵌入式
渲染运行时所需满足的需求、边界、接口语义、实施阶段和验收条件。

本文既保留迁移需求和边界，也记录第一版已经采用的技术决策。接口仍属于项目内部
边界，不承诺稳定的第三方 ABI。

## 2. 设计动机

### 2.1 改造前链路

改造前由定制 Godot 可执行文件拥有进程入口和主循环：

```text
Godot main()
  → 加载 project.godot 和 GDScript 场景
  → render_dmabuf_video.gd 负责应用编排
  → Rust Ayagami GDExtension
  → Godot RenderingServer / SubViewport
  → C++ OpenVTDmaBufCapture
  → RGBA DMA-BUF → RGA → MPP → OutputManager → RTSP
```

该链路已经证明人脸跟踪、Ayagami、Godot、Mali-G610、RGA、MPP 和 WebRTC 可以在
一个进程及其输出链路中工作。它保留为历史验证基线。

### 2.2 当前链路

当前由 VTB 自己的 C++ `main()` 拥有应用生命周期：

```text
VTB C++ main()
  → VtbApplication
  → VideoSource + VideoProcessingRuntime + VideoFrameConsumer
  → FaceFrameConsumer + FacePipeline + Live2DTrackingSession
  → Live2D parameter frame C ABI
  → LibGodotInstance::start()
  → C++ 创建 SceneTree、SubViewport 和 Ayagami 模型
  → C++ 每帧向 Godot 提交已完成的 Live2D 参数并驱动 iteration
  → Headless EGL/GBM / Mali-G610 驱动 Godot RenderingServer 完成离屏绘制
  → C++ 取得已完成帧的 GPU 纹理
  → RGBA DMA-BUF → RGA → MPP → EncodedVideoSink
  → 宿主 MediaOutputSession → OutputManager → RTSP
  → LibGodotInstance::finish_application() → 输出排空 → shutdown()
```

Godot 4.7.1 已提供实验性的 LibGodot Linux 静态库/共享库构建和
`GodotInstance::start()/iteration()` 宿主驱动接口。第一版以 LibGodot 为生命周期
入口；VTB 不另行复制一套 `Main::setup/start/iteration/cleanup`。仅当实机探针证明
官方接口缺少本项目必需语义时，才增加范围受控、可重放的 Godot 补丁。

### 2.3 本设计解决的问题

本设计主要解决工程所有权和可维护性问题：

- VTB C++ 应用统一管理视频输入、人脸推理、渲染、时钟、编码和输出生命周期；
- 产品启动路径不再依赖 GDScript 或 `.tscn` 场景编排；
- Godot 被限制在一个窄的渲染能力边界内，业务模块不直接依赖 Godot 内部单例；
- 启动、运行、故障传播和关闭顺序可以由同一套 C++ 状态机管理；
- 后续音频输入、A/V 同步、UVC/UAC2 和多输出可以由 `VtbApplication` 统一组合；
- Godot 升级影响集中在 VTB 自己维护的适配层和补丁集。

### 2.4 本设计不直接解决的问题

嵌入式 Runtime 本身不承诺：

- 提升 Live2D FPS；
- 降低 RKNN、RGA 或 MPP 的单段耗时；
- 自动实现 Headless EGL；
- 自动消除当前 `glBlitFramebuffer()` GPU copy；
- 自动实现摄像头到浏览器的端到端低延迟；
- 自动解决音频输入和 A/V 同步；
- 自动修复 Ayagami 模型中的空索引 Mesh 警告。

性能变化必须通过同一输入、同一模型、同一输出配置下的前后基准证明。

## 3. 更简单的方案及选择理由

### 3.1 保留 Godot 入口，只把 GDScript 改成 C++ Node

该方案修改量最小，可以移除 GDScript，由 Godot `main()` 创建 C++ 根节点。它能够
解决脚本编排问题，但 VTB 仍不能拥有引擎初始化、逐帧 iteration 和最终退出顺序。

该方案保留为嵌入式 Runtime 开发过程中的中间里程碑和回退路径。

### 3.2 重写独立 Live2D 渲染器

完全移除 Godot 后，需要重新实现或重新集成模型加载、Drawable、Mask、混合模式、
Motion、Expression、Physics、纹理和 OpenGL ES 渲染。当前 Ayagami + Godot 路径已经
通过真实模型验证，因此第一阶段不承担这项重写成本。

### 3.3 选择 LibGodot 嵌入式 Runtime

当前选择在 VTB C++ 应用和 Godot LibGodot C API 之间增加窄适配层。该方案保留已经
验证的渲染结果，同时把产品生命周期交给 VTB C++。Godot 4.7 的 LibGodot 仍是实验
接口，因此必须固定 Godot commit、生成匹配的 GDExtension API 并保留版本适配层。
其主要价值属于工程解耦和生命周期一致性，不能包装成未经测量的性能优化。

## 4. 总体范围

### 4.1 第一版包含

- VTB 自己实现并拥有顶层 C++ `main()`；
- 同一进程内初始化和关闭一个 Godot Runtime；
- 加载当前 `project.godot`、Ayagami GDExtension 和 Live2D 模型资源；
- 用 C++ 创建 `SceneTree`、`SubViewport`、模型节点和必要的渲染资源；
- 用 C++ 应用人脸跟踪参数、视图缩放和位置偏移；
- 通过显式逐帧接口驱动 Godot 主循环；
- 在 `frame_post_draw` 对应的完成边界取得当前帧 GPU 纹理；
- 复用现有 RGBA DMA-BUF、RGA、MPP、OutputManager 和 RTSP 路径；
- 完整的启动、运行、停止、错误传播、指标和资源回收；
- 保留当前 Godot-hosted 路径作为开发期回退基线。

### 4.2 最初版本不包含

- 多个 Godot Runtime 实例；
- Runtime 停止后在同一进程内再次初始化；
- 将 Godot 发布为供第三方使用的稳定共享库 ABI；
- 修改 Ayagami Core 的模型计算算法；
- 同时引入 Headless EGL、GBM、Pbuffer 或 Surfaceless EGL；
- 同时引入 SubViewport 外部颜色附件；
- 音频输入、混音和 A/V 同步；
- UVC/UAC2 Gadget 产品验收；
- Windows、macOS 或 x86_64 产品运行支持。

Headless EGL 由 `docs/headless-live2d-uvc-implementation-plan.md` 单独管理。最初版本
沿用已经验证的 X11/EGL + Mali-G610 路径，避免同时改变生命周期和显示后端；
ADR 0002 在生命周期迁移完成后启动独立的显示后端迁移阶段。

## 5. 设计原则

1. **VTB 拥有应用生命周期。** `main()`、退出码、信号处理和顶层状态机属于 VTB。
2. **Godot 只通过窄接口暴露。** 业务模块不能直接调用 `Main::*` 或任意 Godot 单例。
3. **单进程、单 Runtime。** 第一版只允许一个 Godot 全局状态集合。
4. **专用所有者线程拥有 Godot 对象。** SceneTree、RenderingServer、Ayagami 和资源
   加载调用固定在创建 Runtime 的线程。
5. **帧接口符合异步渲染语义。** 参数提交、Godot iteration 和完成帧获取是不同阶段。
6. **不做 CPU 图像回读。** 产品路径不能用 `ViewportTexture::get_image()` 作为连续
   输出接口。
7. **保留当前正确性基线。** 第一版继续使用 `SubViewport GL Texture → GPU copy →
   RGBA DMA-BUF`。
8. **应用负责编排，能力库负责单项能力。** 人脸、渲染、编码和输出不合并成一个
   巨型公共库。
9. **先证明生命周期正确，再优化复制。** 外部颜色附件等优化必须由测量驱动。
10. **Godot 源码差异必须可重放。** 所有修改以明确补丁或独立提交保存，禁止只留在
    远端临时源码目录。

## 6. 目标模块边界

```text
VtbApplication
├── VideoInput
├── VideoProcessingRuntime
│   └── VideoFrameConsumer
├── FacePipeline
├── Live2DFaceMapper
├── GodotRuntime                 VTB-owned facade
│   ├── LibGodotAdapter          only layer allowed to call LibGodot lifecycle API
│   └── Live2DRenderSession      SceneTree/SubViewport/Ayagami ownership
├── DmaBufRenderBridge
├── RgaConverter
├── MppEncoder
├── OutputManager
└── ControlServer
```

### 6.1 `VtbApplication`

负责：

- 解析配置；
- 安装信号处理；
- 创建并按顺序启动各能力模块；
- 维护应用级单调时钟；
- 驱动主循环；
- 决定停止条件和退出码；
- 按逆序关闭所有模块。

`VtbApplication` 作为组合根选择具体的视频、RKNN、Godot 和输出实现，但不承载这些
组件内部的采集、推理、渲染、编码或协议算法。

视频输入和模型管线通过独立工厂函数构造。`VideoProcessingRuntime` 只组合
`VideoSource` 与 `VideoFrameConsumer`；`FaceFrameConsumer` 在人脸模块内适配
`FacePipeline`。`VtbApplication` 只拥有并排序这些边界的生命周期。

### 6.2 `GodotRuntime`

`GodotRuntime` 是带替身后端的生命周期契约原型，目前仅由状态机单元测试使用。
产品使用 `apps/vtb-live2d/vtb_application.cpp` 中的 `LibGodotInstance`。以下条目保留设计职责：

- 初始化 Godot 全局运行环境；
- 创建并持有 SceneTree；
- 加载项目设置和 GDExtension；
- 创建 `Live2DRenderSession`；
- 驱动一次 Godot iteration；
- 暴露已经完成的渲染帧；
- 有序销毁 Godot 对象并清理全局单例。

Godot 输入接口只接受 `OpenVTLive2DFrameInput`。该结构包含最终 Live2D 参数、取景
状态、单调时钟结果产生时间戳和 snapshot_generation；接口不接受摄像头句柄、模型路径、关键点、
RKNN context 或 `FacePipeline` 对象。

它拥有系统资源和 Godot 全局状态，复制和移动操作必须明确删除。

### 6.3 `LibGodotAdapter`

该适配层是唯一允许接触 `libgodot_create_godot_instance()`、`GodotInstance` 和
`libgodot_destroy_godot_instance()` 的代码。Godot 版本变化只能在该层处理，其他
VTB 模块不得包含 Godot `main/` 或 `core/` 私有头文件。

适配层必须复用 LibGodot 原有初始化与清理逻辑，不能复制一份独立的半套启动流程。
若 Godot 4.7.1 共享库没有导出宿主所需的最小方法，补丁只允许增加薄 C ABI 转发，
不得复制 `Main` 初始化实现。

### 6.4 `Live2DRenderSession`

负责：

- 创建 1920×1080 `SubViewport`；
- 加载 Ayagami 模型、Motion、Expression、Physics 和材质资源；
- 缓存模型支持的参数和合法范围；
- 应用一帧 `Live2DParameters`；
- 应用 `zoom`、`offset_x` 和 `offset_y`；
- 管理等待绘制和完成帧状态；
- 提供 SubViewport RID、纹理 RID、原生纹理句柄和关联帧号；
- 在销毁前释放模型和所有 SceneTree 节点。

它不负责摄像头采集、人脸推理、颜色转换、编码或网络发布。

### 6.5 `DmaBufRenderBridge`

第一版复用当前 `OpenVTDmaBufCapture` 的已验证语义：

```text
SubViewport native GL texture
  → glBlitFramebuffer
  → RGBA DMA-BUF slot
  → RGA NV12
  → MPP
```

该模块继续负责 DMA-BUF 槽位、GPU copy、队列背压和完成帧所有权。后续如实现外部颜色
附件，应替换该模块内部实现，不能改变 `VtbApplication` 和 `GodotRuntime` 的接口。

## 7. Runtime 生命周期和状态机

第一版状态机固定为：

```text
CREATED
  → INITIALIZING
  → INITIALIZED
  → MODEL_LOADING
  → READY
  → RUNNING
  → STOPPING
  → STOPPED

任意活动状态
  → FAILED
  → STOPPING
  → STOPPED
```

要求：

- `initialize()` 只允许调用一次；
- `load_model()` 只允许在 `INITIALIZED` 状态调用；
- `start()` 只允许在 `READY` 状态调用；
- `submit_parameters()` 和 `step()` 只允许在 `RUNNING` 状态调用；
- `request_stop()` 可由主线程、信号处理转发逻辑或错误处理逻辑触发；
- `shutdown()` 必须幂等；
- 部分初始化失败时必须回滚已经创建的资源；
- 第一版不支持 `STOPPED → INITIALIZING`；
- Runtime 析构时如果仍未停止，必须执行安全关闭；
- 析构和关闭过程不能抛出异常跨越 `main()` 边界。

## 8. 对外接口语义

### 8.1 初始化配置

Runtime 配置至少包含：

| 字段 | 含义 |
|---|---|
| Godot 资源根目录 | `project.godot`、`.godot` 和 `.gdextension` 的解析根目录 |
| 模型路径 | 当前 `model3.json` 入口 |
| 输出宽高 | 第一版固定验收 1920×1080 |
| 输出 FPS | 第一版固定验收 30 FPS |
| 显示驱动 | 最初版本使用 X11/EGL；ADR 0002 后默认使用 `headless_egl` |
| 渲染驱动 | `opengl3_es` |
| Mali 用户态库路径 | 当前板端匹配的 Rockchip `libmali` |
| 是否启用性能统计 | 控制分段计时和周期报告 |

配置必须是 VTB 自己定义的值类型，不向应用其他模块泄露 `Variant`、`String`、`RID`
或 Godot 容器。

### 8.2 参数提交

参数提交使用项目自有 `Live2DParameters` 类型，至少包含当前十个参数：

- `ParamAngleX/Y/Z`；
- `ParamBodyAngleX/Y/Z`；
- `ParamEyeLOpen/ROpen`；
- `ParamMouthOpenY`；
- `ParamMouthForm`。

提交时同时携带：

- 源人脸帧 ID；
- 源单调时钟时间戳；
- 参数生成时间戳；
- 跟踪是否有效；
- 是否完成校准；
- 置信度。

Runtime 在 Godot 所有者线程应用参数。参数超过配置的最大年龄时，执行当前已经验证的平滑回默认
值策略，不积压历史参数。

### 8.3 逐帧驱动

逐帧接口不能伪装成同步 `render_to_image()`。每个输出帧至少经历：

```text
提交本帧参数
  → 更新 SceneTree / Ayagami
  → Godot iteration
  → RenderingServer 提交 GPU 工作
  → frame_post_draw 完成边界
  → 导出已完成纹理
```

`step()` 由 Godot 所有者线程调用，并使用 `CLOCK_MONOTONIC` 时间。VTB 维护
30 FPS 调度，Godot 内部不能再创建第二套互相竞争的帧率控制。

### 8.4 完成帧

完成帧描述至少包含：

- VTB 渲染帧 ID；
- 关联的人脸源帧 ID；
- 参数提交、Godot iteration、绘制完成的单调时间戳；
- 输出宽高和像素格式；
- SubViewport RID 和纹理 RID，仅限 Godot 适配层内部使用；
- 原生 OpenGL 纹理句柄，仅限 DMA-BUF 桥接层使用；
- 是否为重复参数帧；
- 是否为跟踪失效后的回默认帧。

第一版不得把 Godot `Image` 或完整 RGBA CPU 缓冲作为连续帧接口。

### 8.5 错误接口

每个可能失败的操作必须返回：

- 稳定的 VTB 错误分类；
- 人类可读错误信息；
- 当前 Runtime 状态；
- 可选的 Godot 原始错误码，仅用于诊断。

错误分类至少覆盖：

- 配置无效；
- Godot 初始化失败；
- DisplayServer 或 EGL 初始化失败；
- GDExtension 加载失败；
- 模型或资源加载失败；
- SceneTree/RenderingServer 状态错误；
- 渲染纹理不可用；
- GPU copy、RGA、MPP 或输出失败；
- 非法状态调用；
- 关闭超时。

应用层不得解析 Godot 日志字符串来判断错误类型。

## 9. Godot 源码改造要求

### 9.1 顶层入口

- 新增 VTB 拥有的 C++ `main()`，并链接 ARM64 LibGodot；
- `main()` 只创建 `VtbApplication`、传递参数并返回其退出码；
- VTB `main()` 不直接访问 Godot 全局单例；
- 构建产物使用独立名称，避免与通用 Godot editor 混淆；
- 第一版优先构建 `libgodot.so`，便于保留普通 Godot 基线和独立回退；共享库与宿主仍
  位于同一个进程。验证完成后可以再评估静态链接，不承诺第三方稳定 ABI。

### 9.2 可调用生命周期

通过 LibGodot 形成清晰的可调用阶段：

```text
setup
  → start
  → iteration（重复）
  → cleanup
```

要求：

- 使用 `libgodot_create_godot_instance()` 创建唯一实例；
- 使用 `GodotInstance::start()` 和 `iteration()` 驱动主循环；
- 使用 `libgodot_destroy_godot_instance()` 完成清理；
- 复用 Godot 原有 `Main` 生命周期实现；
- CLI 模式和嵌入模式共享核心初始化代码；
- 不复制 ProjectSettings、DisplayServer、RenderingServer、ResourceLoader 和扩展加载
  的初始化顺序；
- cleanup 能处理 setup 或 start 中途失败；
- 对外只暴露 VTB 需要的最小接口；
- Godot 内部 API 变化不传播到 VTB 其他模块。

### 9.3 SceneTree 与资源加载

- 嵌入模式仍初始化真实 `SceneTree` 和 `RenderingServer`；
- 必须加载当前 Ayagami 所需的混合模式补丁；
- 必须正常发现并加载 Ayagami GDExtension；
- 必须支持 `.model3.json`、`.moc3`、PNG、Motion、Expression、Physics 和 Shader；
- 产品路径不加载 `render_dmabuf_video.gd`；
- 产品路径不要求 `.tscn`，第一阶段允许保留一个无脚本的资源场景用于对照；
- 所有运行时节点最终由 `Live2DRenderSession` C++ 创建和销毁。

### 9.4 正常 Godot 模式兼容

补丁不得无条件破坏原 Godot CLI/editor 入口。构建至少保留以下一种回归方式：

- 从同一源码继续构建正常 Godot editor；或
- 保留未应用嵌入补丁的可复现基线构建。

嵌入式产物和普通 Godot 产物必须使用不同文件名和 manifest。

## 10. 线程与并发要求

### 10.1 线程归属

| 线程 | 允许职责 |
|---|---|
| VTB 进程主线程 | 组件装配、生命周期阶段控制、停止顺序和错误接收 |
| Godot 所有者线程 | Runtime 生命周期、SceneTree、Ayagami、RenderingServer、参数应用 |
| 摄像头线程 | 采集和最新帧发布 |
| 推理线程 | RKNN、PnP、表情和 Live2D 参数生成 |
| 媒体线程 | RGA、MPP、OutputManager 发布 |
| 控制线程 | HTTP 请求解析和向跟踪状态提交控制更新 |

### 10.2 约束

- 推理、媒体和控制线程不得访问 Godot Object、RID、Variant 或 SceneTree；
- 线程间只传递 VTB 自有值类型、资源租约或容量受限的快照；
- 人脸参数继续使用容量一、最新值覆盖的交接方式；
- 视频输出槽位必须有固定容量和明确背压；
- Godot 所有者线程调用 Godot API 时不能持有推理或输出线程需要的锁；
- 关闭时先停止新输入，再排空或放弃有界队列，最后销毁 Godot；
- signal handler 只设置原子停止标志或写入安全通知 fd，不直接调用 Godot API；
- 任何运行线程捕获 `this` 时，对象移动操作保持删除。

## 11. 资源所有权与五法则

拥有下列资源的类型必须明确处理析构、复制构造、复制赋值、移动构造和移动赋值：

- Godot Runtime 全局状态；
- SceneTree 和 Object 引用；
- EGL/OpenGL 句柄；
- DMA-BUF fd 与映射；
- 工作线程；
- socket；
- MPP、RGA 和 RKNN 句柄。

默认策略：

- 独占资源禁止复制；
- Runtime、渲染会话和内部线程对象第一版禁止移动；
- 可由标准 RAII 类型完整表达的所有权遵循零法则；
- 部分初始化使用局部 RAII 守卫，成功后再转移给成员；
- 来源不明的裸指针不得跨模块保存；
- Godot Object 生命周期使用 Godot 支持的引用或对象 ID 规则。

## 12. 配置和启动要求

第一版继续支持当前主要配置项，但由 VTB C++ 解析并生成强类型配置：

- 摄像头设备、宽高、FPS、缓冲数；
- RKNN detector 和 landmark 模型路径；
- Live2D 模型路径；
- 输出宽高和 FPS；
- RTSP 地址；
- 控制口地址、端口和允许 Origin；
- 校准帧数和平滑时间；
- 性能统计开关和报告周期；
- Godot 项目资源根目录；
- 显示和渲染驱动选择。

需求：

- 环境变量可以作为过渡输入；
- 环境变量只在应用启动时解析一次；
- `GodotRuntime` 不直接读取业务环境变量；
- 配置错误在创建摄像头、线程或 Godot Runtime 前返回；
- manifest 记录所有实际生效配置，不记录密码、密钥或认证信息。

## 13. 可观测性要求

### 13.1 逐帧关联标识

每帧至少关联：

```text
camera_frame_id
face_result_id
snapshot_generation
render_frame_id
encoded_frame_id
output_packet_id
```

### 13.2 时间戳

同一进程内统一使用 `CLOCK_MONOTONIC`，至少记录：

- 摄像头 dequeue；
- 推理开始与结束；
- Live2D 参数生成；
- 参数提交给 Runtime；
- Godot iteration 开始与结束；
- `frame_post_draw`；
- GPU copy 开始与结束；
- RGA 开始与结束；
- MPP 开始与结束；
- OutputManager publish；
- 协议实际 write 开始与结束。

### 13.3 统计指标

至少报告：

- 每阶段样本数、平均值、P50、P95、P99 和最大值；
- 摄像头发布、覆盖和丢失帧数；
- 成功跟踪、检测失败、landmark 失败和几何失败数；
- 参数快照复用、过期和回默认数；
- 渲染提交、完成和失败帧数；
- DMA-BUF 空闲/就绪队列深度和背压次数；
- MPP 编码帧、错误和平均码率；
- OutputManager published/written/dropped/errors/reconnects；
- VTB 实际进程 CPU、RSS、线程数；
- Mali、RGA、MPP 和 NPU 可获得的占用信息。

监控必须跟踪真实 VTB 进程 PID，不能采集 `sudo`、shell 或启动器进程。

## 14. 正确性要求

### 14.1 模型和视觉结果

嵌入式路径必须保持：

- 模型尺寸、原点、缩放和位置；
- 十个实时参数的方向、范围和限幅；
- 中立姿态校准；
- Motion、Expression 和 Physics；
- Draw Order；
- Mask；
- Add、Multiply 和 Mix 混合；
- 透明边缘和纹理颜色；
- 当前纵向方向和画面裁剪。

### 14.2 固定输入回归

准备一套不依赖真人摄像头的固定参数序列，至少覆盖：

- 默认姿态；
- 左右、上下和倾斜转头；
- 左右眼分别闭合；
- 张嘴和嘴型变化；
- 模型缩放和位移；
- 一段 Motion；
- 跟踪丢失后的回默认过程。

对当前 Godot-hosted 基线和嵌入式路径分别捕获对应帧。验收时进行逐帧尺寸、像素格式、
关键区域和视觉差异检查。允许的像素误差阈值必须在生成基线后固定并记录，不能在失败后
临时放宽。

## 15. 性能和稳定性验收

### 15.1 测量原则

- 基线和新路径使用同一 RK3588、固件、Mali 库、Godot commit、Ayagami commit、
  Live2D 模型、分辨率、FPS、编码参数和输出端；
- 固定参数序列用于渲染性能对照；
- 真人摄像头用于完整产品链路验收；
- 预热时长、采样时长和统计口径写入 manifest；
- 30 FPS 配置值不能代替实际吞吐测量。

### 15.2 第一版通过条件

第一版至少满足：

1. 单一 VTB 进程完成 C++ 输入、推理、Godot/Ayagami 渲染、RGA、MPP 和 RTSP；
2. 产品运行期间不加载 `.gd`，不启动 Python、FFmpeg CLI 或第二个 Godot 进程；
3. 1920×1080、30 FPS 固定序列连续输出 300 帧，submitted、encoded 和 written
   一致，错误为 0；
4. 真人摄像头完成 30/30 校准并产生有效 Live2D 参数；
5. 浏览器能够建立 WebRTC H.264 会话并持续显示正确模型；
6. 连续运行至少 10 分钟，队列深度有界，无持续内存增长、死锁和线程泄漏；
7. 正常停止后所有 VTB 线程退出，RTSP 端口释放，摄像头 fd、DMA-BUF、RGA、MPP、
   RKNN、EGL 和 Godot 资源释放；
8. 接收 `SIGTERM` 后执行有序关闭并返回约定退出码；
9. 关键渲染阶段 P95 相对冻结基线不得出现未解释的显著退化；具体阈值在基线重跑后
   写入验收 manifest；
10. 视觉回归通过固定参数序列检查。

### 15.3 不允许的替代证明

- 仅编译成功不能代替 RK3588 运行；
- 仅 RTSP 端口打开不能代替浏览器播放；
- 仅编码 30 FPS 不能证明人脸推理达到 30 FPS；
- 仅 `frame_post_draw` 不能证明 GPU 数据已经安全交给下游；
- 仅退出日志不能证明摄像头、端口和设备句柄已经释放；
- 短暂运行不能代替稳定性验收。

## 16. 构建与产物要求

- Godot 和 VTB 原生代码统一在远端编译服务器构建；
- 构建前检查 SSH、远端架构、目标架构、CPU、内存、磁盘、负载和工具链；
- 使用 `/hdd/.fx/toolchains/openvt-dmabuf-rga-mpp-20261027-01` 对应的 Buildroot/Godot
  ARM64 GCC/G++ 13.2.0；
- 使用任务专用构建目录，不修改共享工具链；
- 保存完整命令、退出码和构建日志；
- manifest 至少记录 VTB commit、Godot commit、Godot patch hash、Ayagami commit、
  toolchain、sysroot、构建参数、目标板和产物 SHA-256；
- ARM64 产物使用 `file`、`readelf` 和动态依赖检查确认；
- 部署前在 RK3588 只读核对全部 `DT_NEEDED` 运行库；
- 构建授权不包含重启、刷写或修改设备启动模式。

## 17. 源码和补丁管理要求

建议的代码归属：

```text
apps/vtb-live2d/
  main.cpp
  vtb_application.cpp

include/vtb/render/
  godot_runtime.hpp
  live2d_render_session.hpp

src/render/
  godot_runtime.cpp
  live2d_render_session.cpp

experiments/godot/embedded-runtime/
  patches/
  build/
  README.md
```

实际落盘前可以根据当前 CMake/SCons 组织调整，但必须满足：

- VTB facade 与 Godot 私有适配代码分离；
- Godot patch 可按顺序重复应用；
- patch 中不包含构建产物和绝对用户目录；
- 当前 `render_dmabuf_video.gd` 在迁移完成前保留为行为基线；
- 不直接在远端 Godot 临时源码中形成无法回放的修改；
- 每个阶段使用独立提交，便于回退和二分。

## 18. 分阶段实施计划

### 阶段 0：冻结当前基线

输出：

- 当前 Godot-hosted 可执行文件及 SHA-256；
- 固定参数视觉序列；
- 300 帧渲染、DMA-BUF、RGA、MPP 和 RTSP 指标；
- 真人校准和浏览器播放记录；
- 当前依赖和运行 manifest。

通过条件：现有基线可重复运行和清理。

### 阶段 1：定义 VTB-owned Runtime facade

实现：

- Runtime 配置、状态、错误和帧描述值类型；
- `GodotRuntime` 生命周期接口；
- 不链接 Godot 的 mock 状态机单元测试；
- 资源类五法则检查。

通过条件：非法状态调用、部分初始化失败和幂等关闭测试通过。

### 阶段 2：LibGodot 生命周期探针

实现：

- VTB `main()`；
- `LibGodotAdapter`；
- ARM64 `libgodot.so`；
- 初始化 ProjectSettings、DisplayServer、RenderingServer、SceneTree 和 GDExtension；
- 运行固定次数 iteration；
- 正常 cleanup。

该阶段只渲染简单纯色或基础 Godot 资源，不接 Ayagami、摄像头和媒体输出。

通过条件：RK3588 使用 Mali-G610，运行和关闭后无残留进程、线程和窗口资源。

### 阶段 3：C++ Ayagami RenderSession

实现：

- C++ 创建 SubViewport；
- 加载真实 Ayagami 模型；
- C++ 写入固定 Live2D 参数；
- Motion、Physics、Mask 和混合模式工作；
- 从完成帧取得原生纹理句柄。

通过条件：固定参数视觉回归与当前 GDScript 基线一致。

### 阶段 4：接回 DMA-BUF/RGA/MPP/RTSP

实现：

- 复用当前三槽 RGBA DMA-BUF 和媒体工作线程；
- RGA NV12；
- MPP H.264；
- OutputManager RTSP；
- 完整帧 ID 和时间戳关联。

通过条件：300 帧提交、编码、写出一致，错误和无界队列增长为 0。

### 阶段 5：接入 C++ 人脸链路和控制口

实现：

- 直接 V4L2 摄像头输入；
- LatestFrameSlot；
- RKNN detector/landmark；
- PnP、表情、校准和映射；
- 参数 mailbox；
- `/health`、`/recalibrate` 和 `/view`。

通过条件：真人校准、重新校准、视图控制和跟踪丢失恢复通过。

### 阶段 6：移除产品 GDScript 依赖

实现：

- 产品启动不再加载 `render_dmabuf_video.gd`；
- 产品启动不再依赖 `.tscn` 编排；
- 保留旧路径作为测试和回退入口；
- 更新 README、构建脚本和部署说明。

通过条件：运行 manifest 和进程检查证明产品路径由 VTB C++ `main()` 驱动。

### 阶段 7：稳定性与回归验收

执行：

- 10 分钟以上真实链路；
- 固定参数视觉对照；
- 浏览器 WebRTC；
- 正常停止和 `SIGTERM`；
- 故障注入：模型缺失、摄像头断开、RTSP 不可用、MPP/RGA 初始化失败；
- CPU、RSS、队列、fd 和线程泄漏检查。

通过条件：满足第 15 节全部第一版条件。

## 19. 回退策略

在嵌入式 Runtime 完成验收前：

- 保留当前 Godot-hosted 可执行文件；
- 保留 `render_dmabuf_video.gd` 和对应运行脚本；
- 嵌入式产物使用不同文件名、目录和端口配置；
- 不覆盖板端最后一个已验证版本；
- 每次部署保存旧文件 SHA-256 和恢复命令；
- 嵌入路径失败时回退不要求重新编译。

嵌入式路径通过全部验收后，再单独决定是否删除旧入口。该删除不属于本文第一版实施
范围。

## 20. 第一版已确认的实现决策

Godot 4.7.1 源码检查、ARM64 探针和 RK3588 完整链路验证已经确认：

1. 第一版使用 ARM64 LibGodot 共享库，静态链接留作后续部署优化；
2. 产品资源工程保留一个无脚本空场景，所有业务节点和 Ayagami 混合模式由 C++ 创建；
3. VTB `steady_clock` 是 30 FPS 调度时钟，Godot `--fixed-fps` 只提供一致的模拟步长；
4. Ayagami 通过空资源工程的 GDExtension 清单在 LibGodot 启动时加载；
5. `frame_post_draw` 只作为 Godot 绘制阶段边界，现有 DMA-BUF 路径继续用
   `glFinish()` 保证 GPU copy 完成；
6. LibGodot 使用独立产物名和部署目录，旧 Godot-hosted 路径继续保留；
7. 启动、iteration、输出或关闭错误向 C++ `main()` 传播并决定进程退出码；
8. `SIGINT` 和 `SIGTERM` 的 handler 只设置原子停止标志，进程主线程编排停止，Godot
   资源由所有者线程释放；
9. Godot、GPU copy、RGA、MPP 或输出错误会终止当前产品链路；
10. Headless EGL/GBM 在生命周期迁移完成后作为独立阶段接入，不改变本节记录的
    第一版生命周期结论。

LibGodot 启动失败属于进程级致命错误。Godot 当前清理路径没有为同一进程内反复创建
Runtime 提供足够保证，因此第一版不支持原地重启。

## 21. 完成定义

只有同时满足以下条件，才能宣布“VTB 已实现嵌入式 Godot Runtime”：

- 顶层 `main()` 位于 VTB 自有 C++ 源码；
- VTB 能显式初始化、逐帧驱动和关闭 Godot；
- 产品路径不加载 GDScript；
- Ayagami 模型、Motion、Physics、Mask 和混合结果正确；
- C++ 人脸参数能够持续驱动真实模型；
- GPU 输出不经过持续 CPU 图像回读；
- DMA-BUF、RGA、MPP 和 RTSP 链路通过实机验收；
- 浏览器实际显示正确；
- 长时间运行、错误传播和资源清理通过；
- Godot 源码修改、构建命令、manifest 和回退方式全部可重复。
