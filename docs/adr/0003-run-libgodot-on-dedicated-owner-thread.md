# ADR 0003：在专用所有者线程运行 LibGodot

状态：接受，等待 RK3588 运行时验证
日期：2026-11-12

2026-11-14 结构整理：专用所有者线程的行为保持不变，原应用私有
`GodotRuntimeThread` 拆分为同步任务主体 `GodotRenderPipeline` 和绑定通道、拥有线程的
`GodotRenderStage`，与视频采集和人脸处理的 Pipeline/Stage 装配方式保持一致。

2026-11-14 后续的 [ADR 0004](0004-run-render-and-encode-on-one-owner-thread.md) 将 LibGodot
模块内部的 RGA/MPP 媒体线程合回本 ADR 定义的 Godot 所有者线程。该调整保留 LibGodot
所有权结论，但改变了渲染到编码之间的并发方式和性能边界。

## 背景

ADR 0001 让 VTB 进程主线程直接创建、驱动和销毁唯一 LibGodot 实例。这条路径已经
完成构建与既有运行验证，但应用主线程同时承担组件装配、逐帧调度和关闭编排。后续若
进程主线程需要承载独立的控制事件循环，Godot 的线程所有权会与顶层控制职责重叠。

人脸处理结果通过容量一的 `BoundedFifoChannel<Live2DTrackingSnapshot>` 跨线程交接，
映射计算留在 FaceTrackingStage 线程，Channel 只保存完成后的值。此次调整不
增加第二套参数邮箱，也不允许多个线程拿锁后轮流调用 Godot。目标是让一个专用线程从
创建到销毁独占 LibGodot，其他线程继续只传递 VTB 自有值类型。

## 源码依据

当前保留的 Godot 4.7.1 LibGodot 源码中：

1. `Main::setup()` 在第一阶段结束前调用 `Thread::release_main_thread()`，并说明后续
   `setup2()` 可以由另一个线程接管主线程身份；
2. `GodotInstance::start()` 调用 `Main::setup2()`；
3. `Main::setup2()` 调用 `Thread::make_main_thread()`，把调用它的当前线程登记为 Godot
   主线程。

因此专用线程可以调用 `libgodot_create_godot_instance()` 和
`GodotInstance::start()`，再由同一线程执行 iteration、应用关闭和实例销毁。该结论是
源码确认；Ayagami、Headless EGL、GPU 捕获和完整退出仍需在 RK3588 上验证。

当前产品显式使用 `--render-thread safe`，资源工程同时设置
`rendering/driver/threads/thread_model=1`。Safe 模式下 Godot 所有者线程也是
`RenderingServer` 的 `server_thread`；只有以后切换 Separate 模式时才会新增独立 Godot
Render Thread。

## 决策

应用创建 `GodotRenderPipeline`，再把它交给 `GodotRenderStage`：

```text
VTB 进程主线程
  → 装配摄像头、跟踪、诊断、输出和控制对象
  → GodotRenderPipeline::create() 装配配置与输出依赖
  → 构造并启动 GodotRenderStage，等待 LibGodot 初始化完成
  → 启动跟踪、诊断和控制生产者
  → 放行 Godot 帧循环
  → 等待帧循环结束
  → 停止控制、跟踪和诊断生产者
  → 允许 GodotRenderStage 完成清理并 join

GodotRenderStage 工作线程
  → 接管 GodotRenderPipeline 的独占所有权
  → GodotRenderPipeline::initialize() 创建并启动 LibGodotInstance
  → 通过 try_pop() 非阻塞取得跟踪通道的新快照
  → GodotRenderPipeline::submit() + run_frame()
  → GodotRenderPipeline::finish() 完成 application 和 NV12 生产
  → 同一工作线程销毁 LibGodotInstance 与 Pipeline
```

初始化完成、帧循环结束和允许清理三个生命周期阶段使用互斥锁与条件变量同步。停止请求
继续使用原有原子标志。Godot 运行错误保存在 `exception_ptr` 中，所有者线程结束后由
主线程重新抛出。

`GodotRenderPipeline::create()` 不调用 LibGodot，只保存参数、渲染配置和 NV12 sink 借用。
`GodotRenderStage::thread_main()` 把 Pipeline 的 `unique_ptr` 移入线程局部变量后才调用
`initialize()`。Pipeline 在该线程创建并独占 `LibGodotInstance`，随后内部编排实例完成
渲染和RGBA同步借用；正常退出和异常展开都会在该线程依次销毁实例与Pipeline，维持
创建、使用和销毁的线程归属。

## 考虑过的简单方案

### 保持 LibGodot 在进程主线程

这是代码量最小且已有运行证据的方案。若进程主线程长期只负责 Godot，它仍然是合理的
基线。此次调整的价值是释放顶层主线程职责并形成单独的 Godot 所有权边界，不宣称提高
GPU、RGA、MPP 或 RTSP 性能。

### 多线程共享 LibGodot，并用一个互斥锁串行调用

互斥锁只能防止并发访问，不能把调用线程变成 Godot 登记的主线程。该方案仍会让
SceneTree、RenderingServer 和 EGL/OpenGL 调用来自不同线程，因此不采用。

### 主线程读取快照后再转发到新的 Godot 参数邮箱

`VtbApplication` 已经实例化容量一的跟踪消息通道。Godot 所有者线程可以直接
非阻塞消费，再增加一层邮箱只会增加一次复制和一套生命周期，因此不采用。

## 收益

- LibGodot、SceneTree、Ayagami、RenderingServer 和 EGL/OpenGL 生命周期仍由同一线程
  独占；
- 进程主线程只保留组件装配、阶段控制、停止顺序和错误接收；
- 后续顶层控制事件循环不需要进入 Godot 帧循环；
- 人脸快照交接边界保持不变；渲染与宿主编码模块的同步串联以后续 ADR 0004、0005 为准。

## 代价和边界

- 增加一个线程以及初始化、运行、清理三阶段同步；
- Godot 错误需要跨线程保存和重新抛出；
- LibGodot 仍只支持单实例，同一进程内失败后重启仍未支持；
- 本 ADR 本身不减少 GPU copy，也不保证提高帧率；RGA 与宿主 MPP 的后续边界见 ADR 0004、0005；
- RK3588 上的 Ayagami 加载、渲染正确性、性能、信号退出和资源释放尚未验证。

## 验证指标

1. ARM64 交叉构建和当前裁剪 LibGodot 链接成功；
2. `GodotRenderStage` 工作线程内完成 LibGodot initialize/start，`Live2DRenderSession`
   的主线程检查通过；
3. 300 帧 submitted、encoded、written 数量一致；
4. 输出画面与主线程基线一致；
5. `SIGINT`、`SIGTERM` 和迭代上限三种退出方式均能 join 全部线程；
6. 退出后 DRM、DMA-BUF、摄像头、RTSP 端口和线程全部释放；
7. 对比基线记录帧时间、deadline miss、媒体背压和输出队列指标，不预设性能改善。
