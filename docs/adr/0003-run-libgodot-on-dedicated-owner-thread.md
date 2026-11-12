# ADR 0003：在专用所有者线程运行 LibGodot

状态：接受，等待 RK3588 运行时验证
日期：2026-11-12

## 背景

ADR 0001 让 VTB 进程主线程直接创建、驱动和销毁唯一 LibGodot 实例。这条路径已经
完成构建与既有运行验证，但应用主线程同时承担组件装配、逐帧调度和关闭编排。后续若
进程主线程需要承载独立的控制事件循环，Godot 的线程所有权会与顶层控制职责重叠。

人脸处理结果已经通过 latest-only 快照跨线程交接。该交接随后明确命名为
`LatestTrackingChannel`，映射计算留在人脸消费线程，Channel 只保存完成后的值。此次调整不
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

新增应用私有的 `GodotRuntimeThread`：

```text
VTB 进程主线程
  → 装配摄像头、跟踪、诊断、输出和控制对象
  → 启动 GodotRuntimeThread，并等待 LibGodot 初始化完成
  → 启动跟踪、诊断和控制生产者
  → 放行 Godot 帧循环
  → 等待帧循环结束
  → 停止控制、跟踪和诊断生产者
  → 允许 GodotRuntimeThread 完成清理并 join

GodotRuntimeThread
  → 创建并启动 LibGodotInstance
  → 直接读取 LatestTrackingChannel 的现有最新快照
  → submit + iteration + 完成帧捕获
  → 完成 Godot application
  → 完成主视频输出
  → 销毁 LibGodotInstance
```

初始化完成、帧循环结束和允许清理三个生命周期阶段使用互斥锁与条件变量同步。停止请求
继续使用原有原子标志。Godot 运行错误保存在 `exception_ptr` 中，所有者线程结束后由
主线程重新抛出。

## 考虑过的简单方案

### 保持 LibGodot 在进程主线程

这是代码量最小且已有运行证据的方案。若进程主线程长期只负责 Godot，它仍然是合理的
基线。此次调整的价值是释放顶层主线程职责并形成单独的 Godot 所有权边界，不宣称提高
GPU、RGA、MPP 或 RTSP 性能。

### 多线程共享 LibGodot，并用一个互斥锁串行调用

互斥锁只能防止并发访问，不能把调用线程变成 Godot 登记的主线程。该方案仍会让
SceneTree、RenderingServer 和 EGL/OpenGL 调用来自不同线程，因此不采用。

### 主线程读取快照后再转发到新的 Godot 参数邮箱

`Live2DTrackingSession` 已经提供 mutex 保护的 latest-only 快照。Godot 所有者线程可以
直接读取它，再增加一层邮箱只会增加一次复制和一套生命周期，因此不采用。

## 收益

- LibGodot、SceneTree、Ayagami、RenderingServer 和 EGL/OpenGL 生命周期仍由同一线程
  独占；
- 进程主线程只保留组件装配、阶段控制、停止顺序和错误接收；
- 后续顶层控制事件循环不需要进入 Godot 帧循环；
- 人脸快照、诊断输出和编码输出的既有交接边界保持不变。

## 代价和边界

- 增加一个线程以及初始化、运行、清理三阶段同步；
- Godot 错误需要跨线程保存和重新抛出；
- LibGodot 仍只支持单实例，同一进程内失败后重启仍未支持；
- 该调整不减少 GPU copy，不改变 RGA/MPP 编码，也不保证提高帧率；
- RK3588 上的 Ayagami 加载、渲染正确性、性能、信号退出和资源释放尚未验证。

## 验证指标

1. ARM64 交叉构建和当前裁剪 LibGodot 链接成功；
2. 工作线程内完成 LibGodot initialize/start，`Live2DRenderSession` 的主线程检查通过；
3. 300 帧 submitted、encoded、written 数量一致；
4. 输出画面与主线程基线一致；
5. `SIGINT`、`SIGTERM` 和迭代上限三种退出方式均能 join 全部线程；
6. 退出后 DRM、DMA-BUF、摄像头、RTSP 端口和线程全部释放；
7. 对比基线记录帧时间、deadline miss、媒体背压和输出队列指标，不预设性能改善。
