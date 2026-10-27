# OpenVT + Godot + Ayagami 在 RK3588 上的构建与验证记录

日期：2026-10-27

## 1. 文档范围

本文记录已经实际执行的工作，包括：

- 检查 RK3588 的 GPU、Xorg、Mesa 和 Rockchip 用户态图形库；
- 在远端 x86_64 构建机上交叉编译定制 Godot ARM64；
- 在远端构建机上交叉编译支持 Rockchip 的 `armsoc` Xorg 驱动；
- 在 RK3588 上安装并启用 `armsoc`；
- 使用 CPU、Mali-G610 窗口路径和 `SubViewport` 离屏路径测试 Ayagami 模型；
- 验证 Godot 纹理经 GPU copy 写入 RGBA DMA-BUF，再由 RGA 转 NV12 DMA-BUF、MPP 编码 H.264；
- 记录当前系统改动、产物、哈希、已知警告和回滚方式。

本文只描述已经发生并有日志或产物证明的结果。OpenSeeFace、Headless EGL、
多缓冲连续管线和 UVC 输出尚未接入。

## 2. 当前结论

已经验证以下链路可以工作：

```text
Godot 4.7.1 custom build
    ↓
Ayagami GDExtension
    ↓
Live2D model3/moc3/PNG
    ↓
Rockchip libmali EGL/OpenGL ES 3.2
    ↓
Mali-G610
    ↓
1920×1080 SubViewport
    ↓ 同一 EGL Context 内 glBlitFramebuffer
RGBA DMA-BUF
    ↓ RGA
NV12 DMA-BUF
    ↓ MPP
H.264 / MP4 文件
```

1920×1080 `SubViewport` 离屏测试结果：

```text
600 帧
56.478 FPS
17.706 ms/帧
GPU 平均负载 24.93%
GPU 最高负载 41%
GPU 最高频率 900 MHz
峰值 RSS 797324 KiB
```

当前 GPU 路径仍通过 Xorg 为 Godot 提供 Native Window 和 EGL Window Surface。最终无 Xorg 的 Headless EGL 路径还没有实现。

## 3. 开始前已有的环境

### 3.1 RK3588

```text
主机名：rk3588-ubuntu
架构：aarch64
内核：5.10.110-qem01-linux-probe1
GPU：Mali-G610
内核 GPU 驱动：bifrost_kbase g13p0
GPU 设备：/dev/mali0
显示 DRM：rockchip-drm
```

Rockchip 用户态 Mali 库已经存在：

```text
/opt/rockchip/libmali-g13p0-x11-gbm-1.9-1/usr/lib/aarch64-linux-gnu/
```

主要文件包括：

```text
libmali.so.1.9.0
libmali-hook.so.1.9.0
mali/libEGL.so.1
mali/libGLESv2.so.2
mali/libgbm.so.1
```

### 3.2 OpenVT、Ayagami 和模型

OpenVT 测试目录：

```text
/home/huangnv/open-vt-test
```

Ayagami ARM64 GDExtension：

```text
/home/huangnv/open-vt-test/thirdparty/ayagami/addons/ayagami/lib/
  libayagami_gd.release.aarch64.so
```

Live2D 模型入口：

```text
/home/huangnv/open-vt-model/阿芙洛狄忒模型文件/fense_vts/fense.model3.json
```

模型加载后的观测数据：

```text
模型尺寸：6666×6666
Mesh 数量：250
参数数量：168
主要纹理：8192×8192 PNG
```

## 4. 定制 Godot ARM64 构建

### 4.1 构建原因

Ayagami 需要 Godot 中额外的混合模式注册接口：

```text
RenderingServer.register_blend_mode
```

普通 Godot 4.7.2 可以发现 GDExtension，但缺少 Ayagami 所需的接口。因此使用 Ayagami 补丁编译定制 Godot。

### 4.2 构建机和隔离环境

```text
远端构建机：10.191.32.121
主机名：yons-desktop
主机架构：x86_64
容器：Fedora 43
构建资源：16 CPU / 48 GiB
目标架构：ARM64
```

远端任务目录：

```text
/tmp/openvt-godot-arm64-20261027-01
```

### 4.3 源码和工具链

```text
Godot tag：4.7.1-stable
Godot commit：a13da4feb8d8aefc283c3763d33a2f170a18d541
Godot 源码 SHA256：eb2378d76964b25d5ecb61e16c934b59ae4f605c42cdb1a2a49eb7bf6d33dcf0
Ayagami patch SHA256：e218786c393bcd2f469823b0541383541b66e431c0f89ce55461b0a4af34ee9a
交叉编译 GCC/G++：13.2.0 Buildroot
```

### 4.4 构建命令

```bash
scons -j16 \
  verbose=yes \
  warnings=no \
  progress=no \
  redirect_build_objects=no \
  platform=linuxbsd \
  arch=arm64 \
  target=editor \
  production=yes \
  lto=none \
  accesskit=no \
  wayland=no
```

第一次启动构建命令时，容器缺少 `/usr/bin/time`，构建在实际编译前以 127 退出。只在构建容器中补装 `time` 后重新执行，最终成功。

```text
成功构建耗时：752.69 秒
最终退出码：0
```

### 4.5 Godot 产物

```text
文件：godot-ayagami-4.7.1-linux.arm64
SHA256：463409e108938e8ab1a128fc7457095db1f0c62b11aa2157804e2628288974b1
```

本机位置：

```text
build/remote-artifacts/openvt-rk3588-20261027/godot/
```

RK 位置：

```text
/home/huangnv/tools/godot-ayagami-4.7.1/
```

RK 验证结果：

```text
4.7.1.stable.custom_build
Ayagami blend modes registered
```

## 5. CPU 软件渲染验证

为了先验证模型、Ayagami 和参数更新逻辑，使用环境变量强制 Mesa 软件渲染：

```text
LIBGL_ALWAYS_SOFTWARE=1
MESA_LOADER_DRIVER_OVERRIDE=llvmpipe
```

这两个变量只作用于测试进程，没有写入系统配置。

最初测试脚本错误地减去了 `model.origin`，导致角色被裁切在左上角。测试脚本后来改为：

```gdscript
model.position = viewport_size * 0.5
```

有效 CPU 结果：

```text
渲染器：Mesa llvmpipe
分辨率：1024×717
采样帧数：600
耗时：49.405957 秒
FPS：12.144
每帧：82.343 ms
峰值 RSS：815776 KiB
```

截图：

```text
build/remote-artifacts/openvt-rk3588-20261027/benchmark/
  openvt-cpu-centered-20261027.png
```

SHA256：

```text
8d49bb541291be2c560122740e1c5486b098516bdabf65154eb27cd1ad52aa38
```

## 6. GPU 路径问题定位

原 Xorg 使用通用 `modesetting` 驱动。日志中出现：

```text
Refusing to try glamor on llvmpipe
glamor initialization failed
AIGLX: Screen 0 is not DRI2 capable
```

Godot 使用普通 `opengl3` 时走 Mesa GLX，仍然得到 llvmpipe。使用 `opengl3_es` 尝试 Rockchip EGL 时出现：

```text
EGL version is too old! 1.0 < 1.4
```

定位结论：

```text
bifrost_kbase 和 libmali 已存在；
Godot 与 Ayagami 可以加载；
缺口位于 Xorg 到 Rockchip DRI2/libmali 的连接层。
```

## 7. Rockchip armsoc ARM64 构建

### 7.1 选择源码

Ubuntu 仓库中的 `xserver-xorg-video-armsoc-endlessm` 曾被短暂安装用于静态检查。该包只显示 Exynos/PL111 支持，没有 Rockchip 后端，没有被 Xorg 加载，检查后已经卸载。

最终使用：

```text
仓库：mth/xf86-video-armsoc
commit：f066764f775464ac7fe6ca291378871ec917595e
源码归档 SHA256：3333ae848543dbd2e94f2c010db8ad8d7da76acc4417ab663237b69ca280ca31
```

源码中包含：

```text
src/drmmode_rockchip/drmmode_rockchip.c
```

### 7.2 构建环境

```text
远端构建机：10.191.32.121
容器：Ubuntu 24.04 amd64
构建资源：8 CPU / 8 GiB
目标架构：ARM64
```

远端任务目录：

```text
/tmp/openvt-armsoc-arm64-20261027-01
```

交叉编译依赖：

```text
aarch64-linux-gnu-gcc 13.3.0
xserver-xorg-dev:arm64 2:21.1.12-1ubuntu1.9
libdrm-dev:arm64 2.4.125-1ubuntu0.1~24.04.2
libpixman-1-dev:arm64 0.42.2-1build1
libudev-dev:arm64 255.4-1ubuntu8.18
```

### 7.3 配置与构建

```bash
CC=aarch64-linux-gnu-gcc \
PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig \
./autogen.sh \
  --host=aarch64-linux-gnu \
  --build=x86_64-linux-gnu \
  --prefix=/usr \
  --libdir=/usr/lib/aarch64-linux-gnu \
  --with-xorg-module-dir=/usr/lib/aarch64-linux-gnu/xorg/modules

make -j8 V=1
```

第一次构建失败：

```text
fatal error: libudev.h: No such file or directory
```

只在构建容器中补装 `libudev-dev:arm64` 后重新构建，最终退出码为 0。

### 7.4 armsoc 产物

```text
文件：armsoc_drv.so
架构：ELF64 AArch64
SHA256：587c079ac93eeeb2511b99bce03bf4067afc17f82791ba9fe5de71cb55d80768
```

依赖检查：

```text
libdrm.so.2
libpixman-1.so.0
libc.so.6
ld-linux-aarch64.so.1
```

本机产物：

```text
build/remote-artifacts/openvt-rk3588-20261027/armsoc/
```

RK 暂存目录：

```text
/home/huangnv/openvt-armsoc-driver-20261027/
```

## 8. RK 系统改动

在用户明确同意执行后，新增以下文件：

```text
/usr/lib/xorg/modules/drivers/armsoc_drv.so
/etc/X11/xorg.conf.d/20-armsoc.conf
```

配置内容：

```text
Section "Device"
    Identifier "Rockchip Graphics"
    Driver "armsoc"
    Option "DRI2" "true"
EndSection
```

对应 SHA256：

```text
armsoc_drv.so：587c079ac93eeeb2511b99bce03bf4067afc17f82791ba9fe5de71cb55d80768
20-armsoc.conf：5823f91fbc90ac4d779264c9807f1aa6bea20d33227d5c6063d6006c6151c231
```

安装前系统不存在同名驱动和配置，所以没有旧文件需要覆盖。安装前 Xorg 日志保存在：

```text
/home/huangnv/openvt-xorg-backup-20261027/Xorg.0.before-armsoc.log
```

随后重启了一次 LightDM。当前状态：

```text
LightDM：active
Xorg：active
DriverName：[rockchip]
ARMSOC(0): [DRI2] Setup complete
```

没有执行以下操作：

- 没有刷写固件；
- 没有更换内核；
- 没有修改设备树；
- 没有替换 `bifrost_kbase`；
- 没有全局替换 Ubuntu 的 `libEGL` 或 `libGLES`；
- 没有锁定 CPU、GPU 或 NPU 频率。

## 9. Mali-G610 GPU 验证

测试进程使用隔离的厂商库路径：

```text
LD_LIBRARY_PATH=/opt/rockchip/libmali-g13p0-x11-gbm-1.9-1/usr/lib/aarch64-linux-gnu/mali:\
/opt/rockchip/libmali-g13p0-x11-gbm-1.9-1/usr/lib/aarch64-linux-gnu

MALI_DEFAULT_WINSYS=x11
```

Godot 输出：

```text
OpenGL API OpenGL ES 3.2 v1.g13p0-01eac0
Compatibility - Using Device: ARM - Mali-G610
```

这证明渲染命令实际进入 Mali-G610，而不是 Mesa llvmpipe。

### 9.1 X11 窗口路径

```text
分辨率：1024×717
采样帧数：600
结果：16.171 FPS
每帧：61.839 ms
GPU 忙碌采样：164/174
GPU 最高负载：28%
GPU 最高频率：600 MHz
```

该测试用于验证 Xorg、DRI2、libmali 和 Mali-G610 的连通性，不代表最终 UVC 路径。

### 9.2 真实 SubViewport 离屏路径

新增测试文件：

```text
/home/huangnv/open-vt-test/thirdparty/ayagami/offscreen_benchmark.gd
/home/huangnv/open-vt-test/thirdparty/ayagami/offscreen_benchmark.tscn
```

关键配置：

```gdscript
render_viewport.size = Vector2i(1920, 1080)
render_viewport.disable_3d = true
render_viewport.transparent_bg = false
render_viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
```

宿主 X11 窗口设置为 1×1，并放到 1024×768 虚拟屏幕范围外。Live2D 画面只绘制到 1920×1080 `SubViewport`。

结果：

```text
分辨率：1920×1080 RGBA
采样帧数：600
耗时：10.623699 秒
FPS：56.478
每帧：17.706 ms
GPU 忙碌采样：49/57
GPU 平均负载：24.93%
GPU 最高负载：41%
GPU 最高频率：900 MHz
峰值 RSS：797324 KiB
```

输出文件：

```text
build/remote-artifacts/openvt-rk3588-20261027/benchmark/
  openvt-offscreen-1920x1080.png
```

SHA256：

```text
2084cab8e845592f6c57e54019a23226a19a1f9f911081434c1462bcfd4fa2ab
```

计时区间包含一次 GPU 读回和 PNG 保存，因此结果不是完全排除读回开销的纯 GPU 数值。

### 9.3 真实人物 DMA-BUF → RGA → MPP 路径

新增 Godot 内置模块和运行场景：

```text
experiments/godot/openvt-dmabuf-capture/
```

模块在 Godot render thread 内用 `viewport_get_texture()` 取得真实
SubViewport render-target RID，再用 `texture_get_native_handle()` 取得 GLES
纹理句柄。人物帧通过 GPU `glBlitFramebuffer` 写入由
`/dev/dma_heap/system-uncached` 分配的 RGBA DMA-BUF。RGA 读取该 fd 并写入
NV12 DMA-BUF，MPP 直接导入 NV12 fd 编码 H.264。原始像素没有经过
`get_image()`、`glReadPixels()`、CPU 映射或 CPU memcpy。

2026-10-27 第 3 次实机运行结果：

```text
模型：阿芙洛狄忒 fense.model3.json
分辨率：1920×1080
帧率与帧数：30 FPS，300 帧
输出时长：9.9999 秒
输出格式：H.264，yuv420p
核心链路吞吐：39.264 FPS
GPU copy 平均：1.244 ms
RGA 平均：2.109 ms
MPP 平均：3.956 ms
GPU 平均负载：19.06%
GPU 最高负载：43%
GPU 最高频率：900 MHz
```

FFmpeg 完整解码无错误，`ffprobe` 统计 300 帧。6 个时间点抽帧哈希均不同，
视觉检查确认人物方向和颜色正确，眨眼、转头、嘴部参数和摇头时间段存在画面变化。

最终 MP4：

```text
build/remote-artifacts/openvt-godot-dmabuf-20261027/run-03/
  openvt-ayagami-dmabuf-1920x1080-30fps.mp4
```

SHA256：

```text
29d43d436c0d084df16a356ccb8bc4890ca6c66a1e53180305266ee059bba7a4
```

这一节记录的是同步基线：一组 RGBA/NV12 DMA-BUF，GPU copy 后调用
`glFinish()`，随后在同一帧调用中串行执行 RGA 和 MPP。它已经完成无 CPU
原始像素读回的真实人物链路，同时仍有一次 GPU 内部纹理复制。后续两阶段
流水版本见 9.4。

### 9.4 三槽 RGBA + 单媒体线程流水版本

在不改变 Godot/Ayagami 渲染结果、RGA 格式转换和 MPP 编码参数的前提下，
将原来的单线程串行链路拆成两个可重叠阶段：

```text
Godot 主线程 / render thread
  渲染 SubViewport
  -> 预留空闲 RGBA FrameSlot
  -> glBlitFramebuffer
  -> glFinish
  -> 提交到有界 FIFO

常驻媒体线程
  FIFO 取出 RGBA FrameSlot
  -> RGA 写入线程独占的 NV12 DMA-BUF
  -> MPP 编码
  -> POSIX write 写入 H.264
  -> 归还 RGBA FrameSlot
```

当前实现使用 3 个 RGBA DMA-BUF FrameSlot 和 1 个共享 NV12 DMA-BUF。MPP
仍保持每帧 `encode_put_frame()` 后阻塞等待 `encode_get_packet()` 的既有同步
语义，避免在没有确认 MPP 输入缓冲释放时机前复用 NV12。队列满时生产端阻塞，
保证本地文件验证不丢帧。停止时先停止接收新帧，再排空 FIFO、等待媒体线程，
最后释放 EGL/GL 和 DMA-BUF 资源。

2026-10-27 在同一台 RK3588、同一个模型、相同 1920x1080@30 输入和 300 帧
验证脚本下得到：

| 配置 | 核心吞吐 | 脚本墙钟吞吐 | GPU copy | RGA | MPP |
|---|---:|---:|---:|---:|---:|
| 同步对照 | 44.924 FPS | 44.683 FPS | 1.129 ms | 2.003 ms | 3.952 ms |
| 流水第 1 轮 | 72.238 FPS | 71.523 FPS | 0.876 ms | 1.604 ms | 4.108 ms |
| 流水第 2 轮 | 78.990 FPS | 78.385 FPS | 0.746 ms | 1.356 ms | 3.971 ms |

两轮流水核心吞吐平均为 `75.614 FPS`，相对同步对照提高约 `68.3%`。两轮
`producer_backpressure_count` 均为 0，最大 ready queue 深度均为 1。第 2 轮
媒体线程平均耗时为 `5.327 ms`，P95 为 `5.506 ms`；queue wait 平均为
`0.014 ms`。

该提升描述稳态吞吐：渲染下一帧与上一帧的 RGA/MPP 可以重叠。单帧端到端
延迟仍包含渲染、GPU copy、排队和媒体处理，不能用媒体线程的 `5.327 ms`
代替完整单帧延迟。

三次运行均得到 1920x1080、30 FPS、300 帧、9.9999 秒的 H.264/yuv420p
文件，FFmpeg 完整解码无错误。同步版与两轮流水版的 H.264、MP4 和 6 个抽帧
全部逐字节一致：

```text
H.264 SHA256：9aa4490029445a69516f321685a21657a5b58eaf501459f4a294ce99809bf0b2
MP4 SHA256：29d43d436c0d084df16a356ccb8bc4890ca6c66a1e53180305266ee059bba7a4
```

本机验证资料：

```text
build/remote-artifacts/openvt-godot-pipeline-20261027-01/
  build/
  run-logs/
  run-pipeline-v1-preview/preview-0120.png
```

RK 同步旧版与流水新版保留在不同目录，通过 `OPENVT_GODOT_BIN` 选择。流水版
Godot ARM64 文件及其 SHA256：

```text
/home/huangnv/tools/godot-ayagami-dmabuf-pipeline-v1-4.7.1/godot-ayagami-dmabuf-pipeline-v1-4.7.1-linux.arm64
3046122ec9ec09d65c1aad066345511ea66191ea02089147152ac892b83f513a
```

当前仍保留 `glFinish()` 作为 GPU 与 RGA 的正确性边界。后续产品化工作包括
显式 fence、实时输入下的最新帧策略、异常注入和长时间稳定性验证。

## 10. 已知警告

### 10.1 空 Mesh 警告

模型加载时会多次出现：

```text
index_array_len==NO_INDEX_ARRAY
Invalid array format for surface
```

Ayagami 最终报告 250 个 Mesh，输出图片视觉检查完整。当前判断为模型中少量空索引 Drawable 触发 Godot 警告，仍需在后续模型正确性检查中确认是否可以安全忽略。

### 10.2 AIGLX armsoc_dri.so 警告

Xorg 日志中出现：

```text
AIGLX error: dlopen of armsoc_dri.so failed
AIGLX error: unable to load driver armsoc
```

该错误属于 Mesa AIGLX 路径。当前 Godot 使用 Rockchip `libmali` 的 EGL/OpenGL ES 路径，并已经通过 GPU 名称、devfreq 负载和频率变化证明 Mali-G610 工作。

没有为了消除日志而创建未经验证的 `armsoc_dri.so` 软链接。

## 11. 当前文件与容器状态

本机完整交付目录：

```text
/Users/huangnv/Desktop/VTB/build/remote-artifacts/openvt-rk3588-20261027/
```

该目录位于已被 `.gitignore` 忽略的 `build/` 下，不进入项目 Git 跟踪。

远端构建机当前仍保留：

```text
/tmp/openvt-godot-arm64-20261027-01
/tmp/openvt-armsoc-arm64-20261027-01
```

以及两个运行但空闲的隔离容器：

```text
openvt-godot-arm64-20261027-01
openvt-armsoc-arm64-20261027-01
```

保留原因是方便在 Headless EGL 适配阶段快速重编译。产物已经复制回本机和 RK，并完成 SHA256 校验。

## 12. 回滚 armsoc 的方法

如果后续需要恢复原来的 Xorg `modesetting` 路径，目标是移除本轮新增的两个文件，然后重启 LightDM：

```text
/etc/X11/xorg.conf.d/20-armsoc.conf
/usr/lib/xorg/modules/drivers/armsoc_drv.so
```

执行回滚前应先确保 SSH 可达，并再次核对这两个文件的哈希和路径。本文只记录回滚目标，没有执行回滚。

## 13. 尚未完成

- 无 Xorg 的 Headless EGL；
- OpenSeeFace 实时参数输入；
- OpenSeeFace/RKNN NPU 迁移；
- UVC Gadget 持续输出；
- 显式 GPU fence、实时输入下的最新帧策略和长时间稳定性；
- systemd 后台服务和开机启动；
- Godot 直接渲染到外部颜色附件，去除当前的一次 GPU copy。
