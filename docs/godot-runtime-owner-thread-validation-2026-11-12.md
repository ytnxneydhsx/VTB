# LibGodot 专用所有者线程验证记录（2026-11-12）

## 结论

当前改造已经完成源码依据核对、Doxygen 更新和 ARM64 交叉编译。`vtb-live2d` 能够链接
2026-11-11 保留的裁剪版 LibGodot，生成的宿主与动态库均为 AArch64 ELF。RK3588 上的
启动、渲染、信号退出和资源释放仍待单独实机验证。

## Godot 线程归属依据

保留的 Godot 4.7.1 源码给出以下调用关系：

```text
libgodot_create_godot_instance()
  → Main::setup()
      → Thread::release_main_thread()

GodotInstance::start()
  → Main::setup2()
      → Thread::make_main_thread()
```

对应源码位于：

- `platform/linuxbsd/libgodot_linuxbsd.cpp`：创建实例时调用 `Main::setup()`；
- `core/extension/godot_instance.cpp`：`GodotInstance::start()` 调用 `Main::setup2()`；
- `main/main.cpp`：`Main::setup()` 末尾释放主线程身份，`Main::setup2()` 把当前调用线程
  登记为 Godot 主线程。

因此 `GodotRuntimeThread` 在线程入口内完成创建和 `start()` 后，该线程成为 Godot
登记的主线程。后续 `submit`、`iteration`、application shutdown 和实例销毁也全部留在
这个线程。

## 当前 RenderingServer 线程模型

Godot 4.7.1 在 `main/main.cpp` 中把
`rendering/driver/threads/thread_model` 的默认值设为 `OS::RENDER_THREAD_SAFE`。只有取值
为 `OS::RENDER_SEPARATE_THREAD` 时，`RenderingServerDefault` 才会创建独立 pump task，
并把 OpenGL 上下文迁移到该线程。

VTB 现在同时通过产品启动参数 `--render-thread safe` 和资源设置
`rendering/driver/threads/thread_model=1` 固定 Safe 模式。在这个模式下：

```text
GodotRuntimeThread
  = Godot 主线程
  = RenderingServer server_thread
  = DMA-BUF OpenGL 捕获调用线程
```

这些名称对应同一个 OS thread ID，不表示线程永久固定在某个物理 CPU core 上。

`RenderingServer::call_on_render_thread()` 会比较当前线程与 `server_thread`。二者相同时
先刷新待处理命令，再直接调用捕获回调；Separate 模式下才会写入 Godot 渲染命令队列。
保留这个调用可以维持图形上下文归属，并为以后单独验证 Separate 模式保留正确入口。

## VTB 交接边界

- 进程主线程负责组件装配、启动阶段、停止顺序和异常接收；
- 人脸消费线程完成映射后发布到 `LatestTrackingChannel`，Godot 所有者线程只读取已完成
  的 latest-only 快照；
- 生命周期阶段通过互斥锁和条件变量同步；
- 停止请求使用原有原子标志；
- Godot 与输出失败通过 `std::exception_ptr` 保存，生产者停止后由进程主线程重新抛出；
- 没有增加第二套参数邮箱，也没有让多个线程轮流访问 Godot 对象。

当前命令行入口仍会在 `wait_for_frame_loop()` 等待渲染结束。这是 CLI 的顶层等待策略，
不再承担 Godot API 调用。以后接入 Qt 事件循环时，可以替换这一等待策略，Godot 的
创建和逐帧驱动无需迁回进程主线程。

## 已完成验证

1. `git diff --check` 通过；
2. `./scripts/generate-video-input-docs.sh` 通过；
3. 生成页面包含 `GodotRuntimeThread`、`LatestTrackingChannel` 和
   `read_snapshot_if_changed()`，没有残留旧的 `Live2DTrackingState` 或
   `poll_snapshot_update()`；
4. 远端使用 Buildroot GCC/G++ 13.2.0、16 路并行，从头编译 `vtb-live2d` 成功；
5. 编译启用 `-Wall -Wextra -Wpedantic`，本次构建没有相关警告；
6. `file`、`readelf` 和 `nm` 已核对目标架构、动态依赖与关键线程/LibGodot 符号；
7. 宿主 SHA-256 为
   `2323568a847dc5b0e9f7f4bced6f177ab7f5f9d669fede0f2561be5dbdfa3582`；
8. 裁剪版 LibGodot SHA-256 为
   `fa8ce6aacc6d023c4c4784e056ea56c5223be37b81205a1022bdd46b0a866ef5`。

构建命令、日志、ELF 信息、符号检查和源码逐文件校验位于
`build/remote-artifacts/vtb-godot-safe-channel-20261112/`。

## 待实机验证

- LibGodot 和 Ayagami 能否在所有者线程内完成初始化；
- Headless EGL/OpenGL、DMA-BUF、RGA、MPP 和 RTSP 全链路画面是否正确；
- 300 帧 submitted、encoded、written 计数是否一致；
- `SIGINT`、`SIGTERM` 和迭代上限退出时线程、摄像头、DMA-BUF 与端口是否释放；
- 与原主线程驱动基线相比的帧时间、deadline miss、CPU/GPU 和背压指标。
