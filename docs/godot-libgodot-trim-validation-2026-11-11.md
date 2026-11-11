# LibGodot Live2D-only 裁剪验证记录（2026-11-11）

## 结论

当前 C++ 主链路只需要 Godot 的 2D 场景、动画、GLES3 离屏渲染、PNG、
GDExtension、Headless EGL 和 OpenVT 定制模块。以这组运行能力作为裁剪边界后，
LibGodot 从 81,685,296 字节降到 38,462,104 字节，减少 52.91%。当前完整
`vtb-live2d` 宿主已能与裁剪库完成 ARM64 编译和链接。

这份结果属于构建验证。尚未把产物部署到 RK3588，也没有加载板端 Ayagami
扩展和模型，因此不能替代实机渲染、帧率、长期稳定性和退出流程验收。

## 裁剪依据

裁剪边界来自产品链路实际使用的类型和资源：

```text
C++ LibGodot host
  -> SceneTree / Node2D / SubViewport
  -> Ayagami GDExtension
  -> AnimationPlayer / AnimationLibrary
  -> ArrayMesh / MeshInstance2D / ShaderMaterial
  -> PNG texture
  -> GLES3 + Headless EGL
  -> OpenVT DMA-BUF capture
```

保留项：

- LinuxBSD ARM64 shared-library 模板；
- GLES3、Headless EGL、GBM/Mali；
- 2D 场景、动画、网格、着色器和 SubViewport；
- PNG 解码、GDExtension 和资源加载核心；
- `openvt_dmabuf_capture` 定制模块；
- `--path` 资源根目录覆盖能力。

关闭项：

- 3D 场景节点；
- Godot 2D/3D 物理和导航；
- XR、高级 GUI；
- Vulkan、X11、Wayland；
- ALSA、PulseAudio 和桌面集成；
- Brotli、Minizip；
- 默认可选模块，仅显式启用 OpenVT 模块。

Godot 的 Dummy Audio 和 AudioServer 仍属于引擎核心编译单元。继续删除它们需要
维护额外的 Godot 内核补丁，当前阶段没有把这类侵入式改造纳入产品配置。

## 源码适配

`disable_3d=yes` 会在 Godot 中定义 `_3D_DISABLED`，同时移除
`SubViewport::set_disable_3d()`。OpenVT 渲染会话现在只在保留 3D API 的构建中
调用该函数；裁剪构建已经在编译期关闭 3D，运行语义保持一致。

可复现入口：

```text
experiments/godot/embedded-runtime/build-trimmed-libgodot.sh
```

完整参数和环境变量见同目录 `README.md`。

## 构建环境

- 构建服务器：`fx@10.191.32.121`，x86_64，80 个逻辑 CPU；
- 并行度：24；
- Godot：4.7.1-stable，`a13da4feb8d8aefc283c3763d33a2f170a18d541`；
- VTB 基线：`807642fa5d3ddd3df33d692107e11a2a8ceb2885`；
- 编译器：Buildroot/Godot GCC/G++ 13.2.0；
- 远端任务目录：
  `/hdd/fx-builds/vtb-libgodot-trim-20261111-UW5er1`。

## 结果

| 项目 | 基线 | 裁剪结果 |
|---|---:|---:|
| LibGodot 字节数 | 81,685,296 | 38,462,104 |
| 减少字节数 | — | 43,223,192 |
| 减少比例 | — | 52.91% |

裁剪库 SHA-256：

```text
fa8ce6aacc6d023c4c4784e056ea56c5223be37b81205a1022bdd46b0a866ef5
```

构建与静态检查已确认：

- LibGodot 和 `vtb-live2d` 均为 AArch64 ELF；
- LibGodot 保留全部八个 `openvt_libgodot_application_*` 导出；
- 保留 `DisplayServerHeadlessEGL`；
- 保留 GBM、Mali、RGA 和 Rockchip MPP 动态依赖；
- 没有直接 X11、XCB 或 Wayland 动态依赖；
- `vtb-live2d` 使用裁剪库完成链接，RPATH 为 `$ORIGIN`；
- 远端与 Mac 取回产物的 SHA-256 一致。

本地取回产物及完整 manifest 位于：

```text
build/remote-artifacts/vtb-libgodot-trim-20261111/
```

## 实机验收项

进入替换基线之前，需要单独授权并在 RK3588 完成：

1. Ayagami GDExtension 加载；
2. 当前模型 PNG、MOC3、动作与物理文件加载；
3. Live2D 画面、透明度和三种混合模式对照；
4. submitted、encoded、written 帧数与帧时间对照；
5. 连续运行和退出资源释放；
6. `/proc/<pid>/maps` 中实际加载的是裁剪库。
