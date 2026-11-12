# ADR 0001：使用 LibGodot 承载 VTB 嵌入式渲染 Runtime

状态：接受
日期：2026-10-30

显示后端部分已由 [ADR 0002](0002-use-headless-egl-for-product-runtime.md) 替代；
本文关于 LibGodot 生命周期所有权和适配层边界的决策继续有效。
LibGodot 的具体线程所有权已由
[ADR 0003](0003-run-libgodot-on-dedicated-owner-thread.md) 调整为专用所有者线程。

## 背景

当前产品由定制 Godot 可执行文件拥有 `main()` 和主循环，GDScript 负责创建
Ayagami、启动 C++ 人脸运行时、等待绘制完成并提交 DMA-BUF 输出。该路径已经在
RK3588 验证，但输入、推理、渲染、编码和输出的顶层生命周期仍分散在 Godot 场景与
VTB C++ 模块之间。

迁移目标是让 VTB 自有 C++ `main()` 统一管理启动、逐帧调度、错误传播、信号和逆序
关闭，同时保留已验证的 Godot/Ayagami、X11/EGL、DMA-BUF、RGA、MPP 和 RTSP 实现。

## 决策

第一版使用 Godot 4.7.1 LibGodot Linux 共享库：

```text
vtb-live2d main
  → libgodot_create_godot_instance
  → GodotInstance::start
  → VTB 主循环调用 GodotInstance::iteration
  → libgodot_destroy_godot_instance
```

VTB 通过 `LibGodotAdapter` 隔离实验性 API。Godot 内部类型只能出现在适配层和
`Live2DRenderSession`，输入、推理、输出和应用状态机只使用 VTB 自有类型。

若共享库可见性或无主场景启动缺少必要接口，Godot 补丁只增加薄转发或放宽
LibGodot 专用启动条件。补丁不得复制 `Main` 初始化/清理实现。

## 考虑过的方案

### 保留 Godot `main()`，把 GDScript 改成 C++ Node

改动小、风险低，可以作为迁移中间里程碑和回退路径。它仍让 Godot 决定进程入口、
iteration 调度和最终退出顺序，不能达到本次生命周期所有权目标。

### VTB 直接调用 `Main::*` 私有接口

可以实现目标，但会重复 LibGodot 已经提供的入口，并扩大对 Godot 私有实现的依赖。

### 重写独立 Live2D 渲染器

可以完全移除 Godot，但需要重新承担模型加载、Mask、混合、Motion、Expression、
Physics 和 GLES 渲染正确性，超出当前迁移范围。

## 结果

收益：

- VTB C++ 拥有进程和主循环；
- 产品路径可以移除 GDScript 与场景编排；
- Godot 升级影响集中在适配层和补丁集；
- 音频、A/V 同步和多输出可以在同一应用状态机中继续组合。

代价：

- LibGodot 在 Godot 4.7 仍是实验接口；
- 构建增加 `libgodot.so` 和匹配的 GDExtension API 约束；
- 第一版只支持单实例且不支持同进程重启；
- 需要重新验证 Ayagami 扩展加载、无脚本 SceneTree 和完整资源清理。

## 验证

- 固定 Godot、Ayagami、工具链和补丁哈希；
- 先完成 initialize/start/iteration/destroy 生命周期探针；
- 视觉固定序列与 Godot-hosted 基线对照；
- 300 帧 submitted/encoded/written 一致；
- 真人 30/30 校准与浏览器 WebRTC 通过；
- 连续运行至少 10 分钟；
- 正常停止与 `SIGTERM` 后进程、线程、端口和设备句柄全部释放。
