# VTB C++ 主进程嵌入式 Godot 验收记录

日期：2026-10-30
目标板：RK3588 Ubuntu ARM64，Linux 5.10.110
产品入口：`vtb-live2d`

VTB implementation commit:
`ef7edef75f1c6bdd92305404f55f5fd7eaf6da53`.

## 1. 验收范围

本次验证覆盖 VTB C++ `main()` 拥有的单进程链路：

```text
C++ main / steady_clock / signal
  -> LibGodot start + explicit iteration
  -> C++ FaceTrackingRuntime (V4L2/OpenCV/RKNN/PnP/mapper)
  -> C++ Live2DRenderSession + Ayagami
  -> OpenVTDmaBufCapture
  -> RGA NV12 -> MPP H.264 -> OutputManager -> RTSP
  -> MediaMTX -> browser WebRTC
```

产品资源工程只包含无脚本空场景、Ayagami GDExtension 和资源文件。运行期间没有加载
GDScript，也没有启动 Python、FFmpeg CLI 或第二个 Godot 进程。

## 2. 构建环境和产物

构建服务器为 `fx@10.191.32.121`，架构 `x86_64`。构建前检查到 80 个逻辑 CPU、
125 GiB 内存和 `/hdd` 6.9 TiB 可用空间。任务专用目录：

```text
/hdd/fx-builds/vtb-embedded-runtime-0kRfiJLr
```

使用的 ARM64 编译器为：

```text
/hdd/.fx/toolchains/openvt-dmabuf-rga-mpp-20261027-01/sdk/bin/
  aarch64-godot-linux-gnu-g++
GCC/G++ 13.2.0
```

Godot 固定为 `4.7.1-stable`，commit
`a13da4feb8d8aefc283c3763d33a2f170a18d541`；Ayagami Godot 补丁 SHA-256 为
`e218786c393bcd2f469823b0541383541b66e431c0f89ce55461b0a4af34ee9a`。

LibGodot 关键构建参数：

```text
platform=linuxbsd arch=arm64 target=template_release
library_type=shared_library production=yes disable_path_overrides=no
x11=yes wayland=no vulkan=no opengl3=yes use_static_cpp=no
```

`disable_path_overrides=no` 允许 C++ 宿主通过 `--path` 指定独立产品资源根目录。

产物 SHA-256：

| 产物 | SHA-256 |
|---|---|
| `vtb-live2d` | `9aaceebbe18fbe8ee2a3d96c25291fe7662c38c142407aa0eca0169ae3506a35` |
| `libgodot.linuxbsd.template_release.arm64.so` | `1c9b8ba68118bb4f876fa2959f9ed551d06822714d06202f533fb46b1f52400d` |
| `libayagami_gd.release.aarch64.so` | `b726920f08624c13f83c92561afa8d4d53da4fa493fb463bd5db57418c2ce205` |

`file` 和 `readelf` 确认宿主与 LibGodot 均为 AArch64 ELF。宿主只设置 `$ORIGIN`
RPATH；板端 `ldd` 检查没有未解析依赖。

## 3. 状态机检查

在远端服务器执行：

```text
ctest --test-dir .../vtb-native-build \
  -R godot-runtime-state-machine --output-on-failure
```

结果为 1/1 通过。覆盖合法生命周期、非法状态调用、配置错误、部分初始化失败、参数帧
校验、渲染帧校验、停止请求和幂等关闭。

## 4. RK3588 300 帧链路

配置为 1920x1080、30 FPS、三槽 RGBA DMA-BUF、RGA NV12、MPP H.264、RTSP TCP。
300 帧测量结果：

| 项目 | 结果 |
|---|---:|
| 媒体区间 | 9973.524 ms |
| 实际媒体 FPS | 30.080 |
| submitted / encoded | 300 / 300 |
| published / written | 300 / 300 |
| dropped / errors / reconnects | 0 / 0 / 0 |
| 最大 ready queue 深度 | 1 |
| GPU copy 平均 / P95 / P99 / 最大 | 1.691 / 2.300 / 10.135 / 10.380 ms |
| RGA 平均 / P95 / 最大 | 2.308 / 2.971 / 17.474 ms |
| MPP 平均 / P95 / 最大 | 4.048 / 4.234 / 7.150 ms |
| media 平均 / P95 / 最大 | 6.375 / 7.061 / 21.829 ms |
| RTSP write 平均 / 最大 | 0.202 / 1.069 ms |
| LibGodot iterations / completed frames | 300 / 300 |

主机帧时钟启用后，累计 300 帧与 30 FPS 目标一致。人脸 generation 在第 30 帧为
41，在第 300 帧为 486，证明人脸线程与渲染主循环在同一进程内持续工作。

## 5. 浏览器播放

浏览器访问：

```text
http://192.168.77.2:8889/live2d-cpp-host/
```

浏览器中实际显示了完整的粉发 Live2D 模型。MediaMTX 日志确认来自
`192.168.77.1` 的 WebRTC peer connection 已建立，并读取包含一个 H.264 track 的
`live2d-cpp-host` 流。

## 6. 已知输入与校准

固定已知正脸序列扩展为足以覆盖控制回归的 10000 帧只读符号链接：

```text
/home/huangnv/VTB/runtime/cpp-face-known-input-20261030-long/face%03d.png
```

最终二进制首次完成 30/30 校准后，`/health` 返回：

```json
{"calibrated":true,"tracking_valid":true,"progress":30,"required":30,"confidence":0.919012}
```

`POST /view` 将视图更新为 `zoom=1.35`、`offset_x=0.08`、`offset_y=-0.04`。随后
`POST /recalibrate` 使状态回到 `calibrated=false`、`progress=0`，同一进程再次完成
30/30 校准，最终置信度为 `0.922563`，视图设置保持不变。

固定输入有限运行的媒体结果为：

```text
iterations=600 completed_frames=600
submitted=600 encoded=600 throughput_fps=30.035
published=600 written=600 dropped=0 errors=0 reconnects=0
```

补齐空的 global script cache 后，最终运行中 `Could not load global script cache` 计数为
0。当前物理摄像头画面没有可见人脸，因此其 `/health` 正确保持
`calibrated=false`、`tracking_valid=false`、`progress=0`；该结果没有被当作真人校准
通过记录。

## 7. 持续运行和退出清理

真实摄像头输入连续运行 624 秒。47 秒采样时 RSS 为 710356 KiB，结束前为
710612 KiB，增长 256 KiB；线程数保持 23，结束前 fd 数为 54。最终统计：

```text
iterations=18645 completed_frames=18645
submitted=18645 encoded=18645 throughput_fps=29.992
published=18645 written=18645 dropped=0 errors=0 reconnects=0
max_ready_depth=1
```

向真实 `vtb-live2d` PID 发送 `SIGTERM` 后，主循环打印最终计数并完成逆序关闭。验收脚本
确认进程消失、`192.168.77.2:11575` 释放，并通过 `/proc/*/fd` 检查确认没有进程继续
持有 `/dev/video0`。

## 8. 已知边界

- 当前路径继续使用 X11/EGL 和 Mali-G610；Headless EGL/GBM 单独实施。
- 音频驱动回退到 Godot dummy；音频输入和 A/V 同步不属于本次迁移。
- 当前模型的空索引 Mesh 仍产生 Ayagami/Godot 既有警告。
- LibGodot 启动失败会终止进程；第一版不支持同一进程内销毁后重建 Runtime。
- `frame_post_draw` 只作为绘制阶段边界；当前 DMA-BUF GPU copy 继续用 `glFinish()`
  保证交付完成。
