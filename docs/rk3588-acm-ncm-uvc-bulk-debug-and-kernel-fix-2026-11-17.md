# RK3588 ACM + NCM + UVC 复合 USB 排错与内核修复记录

> 日期：2026-11-17（Asia/Shanghai）
> 工作目录：`/Users/huangnv/Desktop/VTB`
> 设备端：RK3588，Linux `5.10.110-qem01-linux-probe1`
> 主机端：Raspberry Pi 4B
> 目的：记录本次会话从“树莓派识别不到 RK”到三功能枚举、内核修复、刷写和实机出帧验证的完整过程。
> 说明：本文区分已经实机验证的事实、失败实验、推断和后续设计事项。

## 1. 最终结论

本次问题由三层因素叠加形成：

1. 原来的 UVC 使用等时端点。RK3588 当前 DWC3/BSP 在同时链接 UVC、ACM 和 NCM 时无法为 UVC 分配流端点，因此此前的 ACM+UVC 配置移除了 NCM。
2. 改用 Rockchip 的 UVC Bulk 模式后，三个 Function 可以同时分配端点并被树莓派枚举，但项目的 GStreamer `uvcsink` 起初没有读取 Rockchip 的 `streaming_bulk` 属性，也没有按 Bulk 模式填写 Probe/Commit 的最大 payload。
3. 修复 GStreamer 后，UVC 仍在板端 `VIDIOC_STREAMON` 返回 `ENODEV`。最终确认当前 G13 内核缺少 Rockchip 后续发布的两项 Bulk UVC 状态修复。

完成的最终状态：

- RK 内核已由 `#2` 更新为：

  ```text
  Linux 5.10.110-qem01-linux-probe1 #3
  Tue Nov 17 11:30:00 UTC 2026
  ```

- 同一个 USB 配置同时包含：

  ```text
  UVC：接口 0 / 1
  ACM：接口 2 / 3
  NCM：接口 4 / 5
  ```

- Raspberry Pi 实机验证：

  - `/dev/video0`：MJPEG 1920×1080 @ 30 fps；
  - `/dev/ttyACM0`：收到 RK 登录提示；
  - `usb0`：`169.254.4.2/16`；
  - RK `usb0`：`169.254.4.1/16`；
  - NCM ping 4/4，0% 丢包；
  - UVC 连续捕获并成功解码 30 帧；
  - 原来的 `VIDIOC_STREAMON -> ENODEV` 已消失。

仍然存在的用户态生命周期问题：UVC Function 会在用户态 UVC 所有者订阅事件时激活复合 gadget，并在所有者退出时停用它。因此 `vtb-live2d`、`uvcsink` 或独立 UVC 服务均未运行时，树莓派看不到 UVC、ACM 和 NCM。要让 ACM/NCM 永久在线，需要后续增加独立常驻的 UVC Link 服务，并让 Live2D 通过 IPC 向它交付帧。

## 2. 初始现象和用户关心的问题

会话开始时，RK 的 OTG 口已经接到 Raspberry Pi，但树莓派无法发现 RK。此前同样的接法能够识别和通信，因此主要问题包括：

- 两边是否还能互相发现；
- ACM+UVC 的历史改动为什么存在；
- Live2D 退出后，UVC 是否仍占用 OTG；
- 一个 USB 口能否同时提供摄像头、串口和网卡；
- 如果 ACM 还存在，为什么仍不能通过串口通信；
- 修复能否完全在用户态完成；
- 恢复 NCM 是否需要重新编译或刷写内核；
- Rockchip 后续是否有支持多功能复合 USB 的驱动更新。

最终排查证明，问题同时涉及 ConfigFS 配置、用户态 UVC 所有者、GStreamer UVC 协议处理和内核 Bulk UVC 状态机。单独退出 Live2D、单独停止 UVC 分支或只恢复一个 ConfigFS 链接均不能完整解决。

## 3. USB 角色和复合设备行为

### 3.1 主机与设备角色

本次连接中：

```text
Raspberry Pi：USB Host
RK3588 OTG：USB Device / Gadget
```

RK 的 `fc000000.usb` 是 UDC。设备树已把对应控制器配置为 peripheral 模式，因此角色方向正确。

### 3.2 一个 USB 口可以同时有多个功能

ACM、NCM 和 UVC 在这里属于同一个 USB Composite Device 中的多个 Interface。主机完成一次枚举后，会看到全部接口并分别绑定驱动：

```text
Composite Device
├── UVC VideoControl / VideoStreaming
├── CDC ACM Control / Data
└── CDC NCM Control / Data
```

主机不会从三个功能中只挑一个“优先级最高”的功能。接口描述符、端点资源和主机驱动都正常时，三个功能可以同时存在。

### 3.3 用户态程序不能在退出后自动改变 USB 身份

Live2D 停止发送视频，并不意味着 USB 会自动从摄像头切换成串口或网卡。设备对外暴露什么功能由 ConfigFS 和 Gadget 驱动建立的描述符决定。改变功能集合通常需要重新配置 gadget 并触发重新枚举。

当前 UVC Gadget 还有一个额外行为：用户态 UVC 所有者退出时调用 Function deactivate，导致整个复合设备从主机侧消失。于是 ACM 和 NCM 虽然仍保留在 ConfigFS 中，主机也无法继续访问它们。

## 4. 历史配置与第一次根因判断

项目原先存在两种路径：

1. 标准恢复模式：ACM + NCM；
2. Live2D UVC 模式：ACM + UVC。

历史 ACM+UVC 脚本主动移除了 NCM，原因是旧配置使用 UVC 等时传输时，DWC3 无法在 ACM、NCM 已占用端点后继续为 UVC 分配 streaming endpoint。对应的历史错误是：

```text
Unable to allocate streaming EP
```

所以“三个功能不能同时存在”并非 USB Composite 规范限制。当前 BSP 的端点类型和分配顺序造成了资源冲突。

## 5. 排错过程总览

| 阶段 | 操作 | 结果 |
|---|---|---|
| 1 | 检查现有脚本、服务、ConfigFS 和历史记录 | 确认 ACM+UVC 改动曾主动移除 NCM |
| 2 | 分析 UVC/Live2D 生命周期 | 确认 UVC 所有者退出会使整个复合 gadget 消失 |
| 3 | 研究 Rockchip `streaming_bulk` | 确认 Bulk UVC 可降低等时端点压力 |
| 4 | 冷启动建立 UVC+ACM+NCM Bulk 配置 | 三个功能首次同时枚举成功 |
| 5 | 验证 ACM 和 NCM | 登录提示和网络 ping 正常 |
| 6 | 修复 GStreamer Bulk Probe/Commit | 插件成功构建、部署，但 UVC 仍无帧 |
| 7 | 尝试不同 STREAMON 启动时机 | 在 STREAMON 和 COMMIT 两种时机都失败 |
| 8 | 打开 V4L2/GStreamer 详细日志 | 定位板端 `VIDIOC_STREAMON` 返回 `ENODEV` |
| 9 | 查询 Rockchip 官方提交历史 | 找到两项直接对应的 Bulk 状态机修复 |
| 10 | 找回准确的 G13 内核归档 | 配置和旧 Image 与运行中内核完全匹配 |
| 11 | 远端构建、重打包、反向解包校验 | 新 `boot.img` 构建成功 |
| 12 | 备份、写入 boot 分区、`reboot` | 新内核 `#3` 正常启动 |
| 13 | 三功能与 30 帧 UVC 实测 | 全部通过 |

## 6. 冷启动 ACM + NCM + UVC Bulk 配置

### 6.1 新增配置

新增脚本：

```text
support/uvc/start-rk3588-acm-ncm-uvc-bulk-gadget.sh
```

部署到板端：

```text
/usr/local/sbin/rk3588-usb-gadget-acm-ncm-uvc-bulk
```

新增 systemd drop-in：

```text
support/uvc/10-vtb-acm-ncm-uvc-bulk.conf
```

部署到：

```text
/etc/systemd/system/rk3588-usb-gadget.service.d/
  10-vtb-acm-ncm-uvc-bulk.conf
```

脚本在 ConfigFS 为空的冷启动阶段创建完整 gadget，并设置：

```text
idVendor  = 0x1d6b
idProduct = 0x0104
UVC       = MJPEG 1920×1080 @ 30 fps
streaming_bulk = 1
NCM device address = 02:35:88:00:00:01
NCM host address   = 02:35:88:00:00:02
```

Function 链接顺序为：

```text
UVC -> ACM -> NCM
```

这个顺序让 UVC 先取得 Bulk IN endpoint，再让 ACM 和 NCM 分配剩余端点。

### 6.2 为什么要求冷启动

当前 5.10 BSP 曾在运行中解除和重新绑定 UVC Function 时触发内核 warning，并导致 SSH 中断。新脚本因此遵循：

- 只在 ConfigFS gadget 尚不存在时创建；
- 已存在 gadget 时拒绝覆盖；
- 模式切换通过干净重启完成；
- 不在运行中反复 unbind/rebind UVC。

### 6.3 第一次三功能枚举结果

在第一次干净重启后，Raspberry Pi 看到一个 `1d6b:0104` Multifunction Composite Gadget：

```text
If 0/1: Video          -> uvcvideo
If 2/3: CDC ACM        -> cdc_acm  -> /dev/ttyACM0
If 4/5: CDC NCM        -> cdc_ncm  -> usb0
```

此时已证明 Bulk 模式解决了原先的端点分配问题。ACM 和 NCM 均正常：

```text
RK usb0: 169.254.4.1/16
Pi usb0: 169.254.4.2/16
```

NCM ping 成功，ACM 可以收到 RK 登录提示。

## 7. GStreamer Bulk UVC 修复

### 7.1 问题

GStreamer 1.28.9 的 `uvcsink` 只读取标准 UVC ConfigFS 字段，不认识 Rockchip 额外提供的：

```text
streaming_bulk
```

Bulk UVC 的 Probe/Commit 响应需要使用整帧大小作为 `dwMaxPayloadTransferSize`。原插件仍使用 endpoint packet size，协议语义不完整。

### 7.2 补丁

新增：

```text
support/gstreamer/patches/
  0007-uvcsink-rockchip-bulk-streaming.patch
```

修改内容：

- 从 ConfigFS 读取可选的 `streaming_bulk`；
- Bulk 模式使用 `dwMaxVideoFrameSize` 作为最大 payload；
- 没有该 Rockchip 属性的内核继续保持标准行为。

### 7.3 构建和部署

插件通过 Raspberry Pi 上的 `x86-build` 调度到 x86_64 编译服务器，使用项目固定 ARM64 工具链构建。

最终插件：

```text
/opt/vtb/gstreamer-1.28.9/lib/aarch64-linux-gnu/gstreamer-1.0/
  libgstuvcgadget.so
```

SHA-256：

```text
7d93ae51eea3b442bb0de87d5163755edf0a533d9dce9fbb2e5fbc8bb33cff64
```

旧插件保留为：

```text
libgstuvcgadget.so.pre-rockchip-bulk-20261117
```

### 7.4 修复后仍然无帧

修复插件后，主机可以枚举 MJPEG 1920×1080@30，主机侧 `VIDIOC_STREAMON` 也能发起，但捕获文件仍为 0 字节。

因此继续打开 RK 端 GStreamer 和 V4L2 调试日志。

## 8. `VIDIOC_STREAMON -> ENODEV` 的定位

### 8.1 详细日志观察

板端 `/dev/video2` 已确认属于 UVC Gadget：

```text
name          = dwc3-gadget
function_name = uvc.usb0
capability    = Video Output | Streaming
format        = MJPG 1920×1080
sizeimage     = 2073600
```

GStreamer/V4L2 日志显示：

1. 第一个 `VIDIOC_QBUF` 成功；
2. Gadget 侧 `VIDIOC_STREAMON` 返回 `ENODEV`；
3. 后续 QBUF 因流状态失败返回 `EINVAL`；
4. 主机最终超时，得到 0 字节。

### 8.2 尝试过的两个启动时机

尝试 A：收到 Rockchip `UVC_EVENT_STREAMON` 后立即启动内部 `v4l2sink`。

结果：`VIDIOC_STREAMON -> ENODEV`。

尝试 B：延迟到 UVC COMMIT 后再启动内部 `v4l2sink`。

结果仍为：`VIDIOC_STREAMON -> ENODEV`。

这排除了“只要调整 GStreamer 启动时机即可解决”的假设。

### 8.3 内核状态机矛盾

旧内核的 Bulk 路径存在两个互相冲突的行为：

```c
if (uvc->state != UVC_STATE_CONNECTED)
    return -ENODEV;
```

而 Bulk set_alt 路径可能已把状态推进为 `UVC_STATE_STREAMING`。用户态随后调用 `VIDIOC_STREAMON` 时，状态已经是 STREAMING，于是旧判断把一个有效状态误判为无设备。

## 9. Rockchip 官方后续修复

在 Rockchip `develop-5.10` 历史中找到两项直接对应的修复：

1. [`7a4ae778bdf2`：usb: gadget: uvc: fix uvc state for bulk transfer](https://github.com/rockchip-linux/kernel/commit/7a4ae778bdf2124023c65c0789b88b6bb8faf6e0)
2. [`39f343ce4fbc`：usb: gadget: uvc: fix error return in bulk mode](https://github.com/rockchip-linux/kernel/commit/39f343ce4fbc68f6992670a3318f5c961d9f2c86)

修复后的核心判断为：

```c
if (uvc->state == UVC_STATE_DISCONNECTED)
    return -ENODEV;
```

并且无论等时还是 Bulk，成功 STREAMON 后都会把状态设置为：

```c
uvc->state = UVC_STATE_STREAMING;
```

项目内合并回移植补丁：

```text
support/uvc/patches/
  0001-rockchip-uvc-bulk-streamon-state.patch
```

## 10. 为什么必须更新内核镜像

运行中内核配置为：

```text
CONFIG_USB_F_UVC=y
CONFIG_USB_CONFIGFS_F_UVC=y
```

UVC Gadget 驱动编进了内核 Image，没有独立 `.ko` 可以热替换。因此本次修复必须：

1. 使用准确源码和准确 `.config` 重新编译 Image；
2. 重新打包 Android boot v2 镜像；
3. 写入 48 MiB `boot` 分区；
4. 重启进入新内核。

根文件系统、用户数据、U-Boot、trust、vbmeta 和 dtbo 分区均不需要重刷。

## 11. 找回准确的 G13 内核构建归档

一开始检查的归档：

```text
/home/fx/vtb-rknpu-098-20261029-01
```

它是相近的 QEM/RKNPU 构建，但 `.config` 与运行中内核仍存在差异，因此没有使用。

随后找到与当前板完全匹配的归档：

```text
/home/fx/vtb-rknpu-g13-098-20261029-02
```

关键匹配证据：

```text
内核版本：5.10.110
LOCALVERSION：-qem01-linux-probe1
运行配置 SHA-256：46137ffaf1ffae9a7ca19677ea14d9e433d54571545aff3de9423fda24cad248
归档配置 SHA-256：46137ffaf1ffae9a7ca19677ea14d9e433d54571545aff3de9423fda24cad248
```

从当前 boot 分区解出的内核 SHA-256：

```text
4390774aea7755bd87746e13fdabb27e7fa5c6daacba38a349fa4609622eccde
```

归档旧 Image SHA-256：

```text
4390774aea7755bd87746e13fdabb27e7fa5c6daacba38a349fa4609622eccde
```

两者完全一致，证明找到了当前运行内核的准确构建来源。

## 12. boot 分区备份与镜像解析

板端分区布局：

```text
/dev/mmcblk0p7
PARTLABEL=boot
大小=48 MiB
```

写入前完整备份：

```text
/home/huangnv/boot-backups/
  boot-before-uvc-bulk-20261117.img
```

备份 SHA-256：

```text
cf69a84137c9bcc1e09d8cf5d9c3a39d3857706cd9b7b66b951b907e96f92aee
```

这是 Android boot image v2：

```text
kernel size       = 38823944
ramdisk size      = 661185
second size       = 6521856
dtb size          = 247840
page size         = 2048
header version    = 2
OS version        = 12.0.0
OS patch level    = 2022-07
kernel address    = 0x10008000
ramdisk address   = 0x11000000
second address    = 0x10f00000
dtb address       = 0x11f00000
```

重打包时只替换 kernel，以下内容逐字节保留：

- ramdisk；
- second-stage payload；
- DTB；
- cmdline；
- load address；
- page size 和 header version。

## 13. 远端构建过程

### 13.1 构建环境

构建由 Raspberry Pi 上的 `x86-build` 调度到：

```text
Host：10.191.32.121
Architecture：x86_64
CPU：80 logical CPUs
Memory：125 GiB
```

使用原构建容器：

```text
vtb-rk3588-kernel-builder:gcc10-20261029
```

工具链：

```text
aarch64-linux-gnu-gcc-10
Ubuntu 10.5.0-4ubuntu2.1
```

构建并行度保持原值：

```text
-j40
```

### 13.2 临时连接问题

Raspberry Pi 的编译服务器 SSH 配置仍指向失效的本地 SOCKS 代理：

```text
ProxyCommand nc -X 5 -x 127.0.0.1:11082 %h %p
```

本次通过 Mac 建立临时反向 SSH 通道，让 Pi 的 `x86-build` 访问编译服务器。构建完成后已恢复 Pi 原始 SSH 配置和 `x86-build` 配置，并关闭通道。

第一次提交在创建工作区前因公网 SSH 断开而退出，没有开始构建。第二次成功完成编译和镜像校验，但在产物回传阶段公网连接再次中断。远端受管工作区保留了完整结果，经 SHA-256 验证后取回，最后使用：

```text
x86-build gc --all
```

清理唯一的受管临时工作区。共享内核归档未被修改。

### 13.3 可重复构建脚本

新增：

```text
support/uvc/build-rk3588-uvc-bulk-kernel.sh
```

脚本执行以下保护：

1. 校验 G13 源码归档 SHA；
2. 校验当前 `.config` SHA；
3. 校验旧 Image SHA；
4. 校验 ramdisk、second 和 DTB SHA；
5. 复制源码到任务专用目录；
6. 应用 UVC Bulk 状态补丁；
7. 使用原容器和 GCC 10 构建；
8. 打包 Android boot v2；
9. 再次解包新镜像；
10. 用 `cmp` 校验 kernel、ramdisk、second、DTB；
11. 生成 manifest 和 `SHA256SUMS`。

### 13.4 新产物

本地产物目录：

```text
build/remote-artifacts/
  rk3588-uvc-bulk-kernel-20261117/
```

新 Image SHA-256：

```text
ae58135652867b69568726c6cb6120620152833189acbb45fcec2682e36c6ec9
```

新 boot image SHA-256：

```text
37f3d6154275d70b37d97cf09a424bbb6e97bda1dc527425c1887361f5ffe5c6
```

内核版本字符串：

```text
Linux version 5.10.110-qem01-linux-probe1
(root@ubuntu-embedded)
(aarch64-linux-gnu-gcc-10 10.5.0)
#3 SMP PREEMPT Tue Nov 17 11:30:00 UTC 2026
```

对新 `vmlinux` 反汇编确认：

- 状态为 0，即 `DISCONNECTED` 时跳转到 `-ENODEV`；
- `CONNECTED` 和 `STREAMING` 不再被错误拒绝；
- 成功后统一写入 `UVC_STATE_STREAMING`。

## 14. 写入与重启

### 14.1 写入前检查

写入前再次读取整个 boot 分区：

```text
当前分区 SHA-256 = cf69a841...
备份文件 SHA-256 = cf69a841...
```

二者一致。

新镜像在 Mac 与 RK 上的 SHA-256 均为：

```text
37f3d6154275d70b37d97cf09a424bbb6e97bda1dc527425c1887361f5ffe5c6
```

### 14.2 写入

实际写入目标：

```text
/dev/disk/by-partlabel/boot
```

写入 48 MiB 后立即回读整个分区，回读 SHA 与新镜像一致。

### 14.3 重启

执行：

```text
sudo reboot
```

重新上线后：

```text
Linux rk3588-ubuntu 5.10.110-qem01-linux-probe1
#3 SMP PREEMPT Tue Nov 17 11:30:00 UTC 2026
```

`rk3588-usb-gadget.service` 为 active，ConfigFS 状态为：

```text
UDC             = fc000000.usb
streaming_bulk  = 1
functions       = uvc.usb0 acm.usb0 ncm.usb0
video node      = /dev/video2
function_name   = uvc.usb0
```

## 15. 最终实机验证

### 15.1 Raspberry Pi 枚举

```text
Bus 001 Device 010: ID 1d6b:0104
Linux Foundation Multifunction Composite Gadget
```

接口：

```text
If 0 Video          uvcvideo
If 1 Video          uvcvideo
If 2 Communications cdc_acm
If 3 CDC Data       cdc_acm
If 4 Communications cdc_ncm
If 5 CDC Data       cdc_ncm
```

### 15.2 NCM

```text
Pi usb0: 169.254.4.2/16
RK usb0: 169.254.4.1/16
```

结果：

```text
4 packets transmitted
4 packets received
0% packet loss
average RTT 2.546 ms
```

### 15.3 ACM

树莓派 `/dev/ttyACM0` 能收到：

```text
huangnv@rk3588-ubuntu:~$
```

### 15.4 UVC

主机枚举格式：

```text
MJPG
1920×1080
30.000 fps
```

黑帧测试使用 RK FFmpeg 生成 MJPEG，经 GStreamer `uvcsink` 输出。Raspberry Pi 捕获结果：

```text
frames      = 30
capture size = 373830 bytes
decode check = passed
```

板端日志中没有出现：

```text
VIDIOC_STREAMON: No such device
ENODEV
Invalid argument
```

启动日志存在一个既有 Rockchip DRM vblank warning，与 USB/UVC 测试无关，本次没有修改 DRM。

## 16. 失败尝试和被排除的假设

### 16.1 “Live2D 退出后 USB 会自动恢复成串口或网卡”

未成立。USB 功能集合由 ConfigFS 描述符决定，应用退出不会自动改写描述符。当前 UVC 所有者退出还会停用整个复合设备。

### 16.2 “ACM Function 还链接着，所以串口一定能通信”

未成立。ACM 链接存在只证明配置中有这个 Function。整个 gadget 未激活或主机未完成枚举时，不会出现可用的 `/dev/ttyACM0`。

### 16.3 “USB 一个口只能从 UVC、ACM、NCM 中选一个”

未成立。三者可以作为不同 Interface 同时枚举。本次最终验证已证明。

### 16.4 “只调整 Function 顺序就足够”

Function 顺序解决了端点分配问题，但没有解决 GStreamer Bulk 协议字段和内核状态机错误。

### 16.5 “只修改 GStreamer 就足够”

GStreamer 补丁让 Probe/Commit 符合 Rockchip Bulk 语义，但旧内核仍在 `VIDIOC_STREAMON` 返回 `ENODEV`。

### 16.6 “把启动时机从 STREAMON 改到 COMMIT”

两种时机均实测失败，根因位于内核状态判断。

### 16.7 “直接使用相近的内核源码重编译”

被拒绝。第一套归档的配置与运行中内核不完全一致。只有找到 G13 归档，并同时匹配运行配置和当前 boot 内核 SHA 后，才继续构建和写盘。

### 16.8 “运行中解绑并重建 UVC”

当前 BSP 有过内核 warning 和 SSH 丢失记录，因此本次采用冷启动配置和完整 reboot，没有再次热重绑。

## 17. 当前板端修改

### 17.1 Gadget

```text
/usr/local/sbin/rk3588-usb-gadget-acm-ncm-uvc-bulk
/etc/systemd/system/rk3588-usb-gadget.service.d/
  10-vtb-acm-ncm-uvc-bulk.conf
```

### 17.2 GStreamer

```text
/opt/vtb/gstreamer-1.28.9/lib/aarch64-linux-gnu/gstreamer-1.0/
  libgstuvcgadget.so
```

回滚插件：

```text
libgstuvcgadget.so.pre-rockchip-bulk-20261117
```

### 17.3 内核和备份

当前 boot 分区 SHA-256：

```text
37f3d6154275d70b37d97cf09a424bbb6e97bda1dc527425c1887361f5ffe5c6
```

板端备份：

```text
/home/huangnv/boot-backups/boot-before-uvc-bulk-20261117.img
/home/huangnv/boot-backups/boot-uvc-bulk-20261117.img
```

## 18. 回滚方式

只有在明确的维护窗口执行：

```bash
ssh rk3588-direct
sudo dd \
  if=/home/huangnv/boot-backups/boot-before-uvc-bulk-20261117.img \
  of=/dev/disk/by-partlabel/boot \
  bs=4M conv=fsync status=progress
sync
sudo reboot
```

重启后应确认：

```bash
uname -a
sudo dd if=/dev/disk/by-partlabel/boot bs=4M status=none | sha256sum
```

旧 boot 分区预期 SHA-256：

```text
cf69a84137c9bcc1e09d8cf5d9c3a39d3857706cd9b7b66b951b907e96f92aee
```

## 19. 结构化用户态改造

内核修复解决了三功能共存和 UVC Bulk 出帧。USB 恢复接口的生命周期仍与 UVC 用户态所有者耦合。

最终结构：

```text
rk3588-usb-gadget.service
  └── 创建 UVC + ACM + NCM ConfigFS gadget

uvc-link.service（常驻）
  ├── 唯一持有 /dev/video2
  ├── 持续处理 UVC Setup / Probe / Commit / Stream 事件
  ├── 没有 Live2D 帧时保持枚举
  └── 通过命名管道接收 MJPEG 帧

vtb-live2d.service
  └── 只生产视频帧，不直接打开和关闭 UVC Gadget 驱动
```

这样可以实现：

- Live2D 重启不影响 ACM/NCM；
- UVC 的内核事件只有一个长期所有者；
- gadget 描述符生命周期与业务应用生命周期分离；
- 后续可以单独监控和重启 Live2D；
- USB 恢复通道始终保留。

该常驻 UVC Link 已于 2026-11-17 实现并部署。板端
`vtb-uvc-link.service` 已启用开机启动，它通过
`/run/vtb/uvc-link.mjpeg` 读取连续 MJPEG，并作为唯一的
`/dev/video2` 所有者。项目源码已增加 `output.uvc.link_path`；设置该
字段后 Live2D 的 `mppjpegenc` 分支会将已编码帧写入 FIFO。该
C++ 改动后来已完成 ARM64 远程编译、板端部署和实机验证，结果见第 24 节。

## 20. 项目文件清单

新增或修改：

```text
support/uvc/start-rk3588-acm-ncm-uvc-bulk-gadget.sh
support/uvc/10-vtb-acm-ncm-uvc-bulk.conf
support/uvc/build-rk3588-uvc-bulk-kernel.sh
support/uvc/patches/0001-rockchip-uvc-bulk-streamon-state.patch
support/uvc/run-vtb-uvc-link.sh
support/uvc/vtb-uvc-link.service
support/uvc/README.md

support/gstreamer/patches/0007-uvcsink-rockchip-bulk-streaming.patch
support/gstreamer/README.md

apps/vtb-live2d/gstreamer_media_session.cpp
apps/vtb-live2d/gstreamer_media_session.hpp
apps/vtb-live2d/vtb_application.cpp
include/vtb/config/vtb_application_config.hpp
src/config/vtb_application_config.cpp
config/vtb-live2d.example.json
tests/vtb_application_config_test.cpp
```

构建与恢复产物：

```text
build/remote-artifacts/rk3588-uvc-bulk-kernel-20261117/
├── boot-before-uvc-bulk-20261117.img
├── boot-before-unpacked/
├── Image-5.10.110-qem01-linux-probe1-uvc-bulk
├── boot-uvc-bulk.img
├── manifest.txt
├── SHA256SUMS
└── unpack-boot.txt
```

## 21. 验证边界

已经完成：

- Shell 语法检查；
- `git diff --check`；
- GStreamer ARM64 构建；
- Rockchip 内核 ARM64 全量构建；
- Android boot v2 打包与反向解包；
- boot 分区写入前备份；
- 写入后整分区回读校验；
- RK reboot；
- 新内核版本确认；
- Raspberry Pi 三 Function 枚举；
- NCM ping；
- ACM 登录提示；
- UVC 30 帧捕获和解码；
- Mac 枚举 `RK3588 Live2D Composite`、`UVC Camera`、ACM 与 NCM；
- Mac 通过 AVFoundation 捕获并解码 1920x1080 帧；
- 常驻 `vtb-uvc-link.service` 安装、开机启用和运行；
- 停止 MJPEG 生产者后 UVC、ACM 和 NCM 保持枚举；
- Mac 保持连接时重启 RK，常驻服务自动启动并恢复三接口枚举；
- Doxygen 重新生成并包含 `uvc_link_path`；
- 测试进程和远端受管工作区清理。

尚未完成：

- 使用正式 Live2D 画面进行长时间 UVC Bulk 稳定性测试；
- UVC 反复打开/关闭测试；
- Live2D 与 RTSP/UVC 长时间并发测试；
- 常时间 Live2D 重启和主机反复开关摄像头测试。

在常驻 UVC Link 初次部署阶段，ARM64 构建未启动的直接原因是树莓派
`x86-build --json doctor`
连接远程编译服务器失败，返回
`Connection closed by UNKNOWN port 65535`。按项目新的回退规则检查了 Mac，
当前没有 AArch64 Linux GCC/G++、Rockchip board sysroot、FFmpeg sysroot 或可复用的
本地构建镜像。因此该阶段没有生成错误的 macOS/Mach-O 产物，也没有改用树莓派或
RK 本地编译。随后已改为 Mac 直连远端服务器并完成构建、部署，结果见第 24 节。

## 22. Doxygen 检查

新增 `uvc_link_path` 并修改了 GStreamer 媒体出口创建流程，因此已运行
`./scripts/generate-video-input-docs.sh`。生成的
`classvtb_1_1_g_streamer_media_session_config.html` 包含新字段，
`classvtb_1_1_g_streamer_media_session.html` 包含常驻 UVC Link 数据路径说明。

## 23. Mac 主机切换与运行时 UDC 解绑结果

RK 从树莓派主机切换到 Mac 后，第一次检查显示 RK 仍保留
`configured/high-speed`，Mac IORegistry 却没有 `1d6b:0104`。尝试通过
ConfigFS 软件解绑 UDC 时，内核在 UVC 函数清理路径产生 trace，
`gadget_dev_desc_UDC_show` 读取进程进入 D 状态，`systemd-journald` 也出现
D 状态。正常 `reboot` 随后卡在关闭阶段，最终由用户完成断电重启。

该结果证明当前 BSP 不适合使用 UDC 运行时解绑作为日常恢复手段。
冷启动后 UDC 为 `not attached`，直到 `uvcsink` 订阅 UVC 事件；启动常驻
UVC Link 后，Mac 立即完成 reset 和枚举。常驻用户态所有者因此同时
解决了自动枚举和避免危险 UDC 热解绑两个问题。

## 24. Live2D 正向画面、FIFO 解耦与 60 FPS 更新

### 24.1 实际问题与基线

Mac 第一次成功捕获正式 Live2D UVC 画面后发现两个问题：

- Godot 发布的 RGBA DMA-BUF 使用 OpenGL 图像坐标方向，直接转为 NV12 后画面上下颠倒；
- 旧 `vtb-live2d` 将 `mppjpegenc` 直接连接 `uvcsink`，USB 主机发送反压会传回 MJPEG
  编码支路。

旧直连模式运行结束时的计数为：

```text
submitted=15923
h264=15923
mjpeg=4844
raw_dropped=11082
```

渲染端 `live2d.render_total` 平均约 14.5 ms，具备接近 60 FPS 的能力；持续丢帧集中在
MJPEG/USB 支路。Mac 断开且 UVC 配置改为 60 FPS 后，旧二进制在短测中得到
`submitted=899 h264=899 mjpeg=899 raw_dropped=0`，证明 RK3588 渲染和 MPP MJPEG
编码能够跟上目标速率，USB 主机反压才是此前卡顿的主要触发条件。

### 24.2 方向修复

`RgaNv12FrameTransformer` 现在选择
`RgaTransformOperation::rgba_to_nv12_flip_vertical`。`RgaDevice` 通过一次
`improcess()` 同时完成：

```text
RGBA8888 → 垂直翻转 → NV12 BT.601 limited range
```

翻转与颜色转换共用同一次 RGA 提交，没有增加第二次整帧拷贝。板端生成 5 帧正式
Live2D MJPEG 后取回首帧，确认人物方向正立、左右关系保持正常。

### 24.3 60 FPS 与反压隔离

当前数据路径为：

```text
vtb-live2d
  → mppjpegenc 1920x1080@60
  → filesink /run/vtb/uvc-link.mjpeg
  → 常驻 vtb-uvc-link
  → jpegparse
  → 4 帧有界 leaky queue
  → uvcsink /dev/video2
```

该结构让 Live2D 不再直接拥有 `/dev/video2`。USB 主机短时变慢时，Link 的有界队列
丢弃旧压缩帧，避免反压持续传回渲染和编码线程；它保持低延迟和应用生命周期隔离，
不承诺主机在任何线材、端口或摄像头应用下都能消费 60 FPS。

板端同步更新：

- Live2D UVC 输出：`60/1`；
- 常驻 Link 输入：`1920x1080, framerate=60/1`；
- UVC `dwDefaultFrameInterval` 和 `dwFrameInterval`：`166666` 个 100 ns 单位；
- SuperSpeed Bulk `streaming_maxburst`：`15`。

新二进制通过 FIFO 与 Link 并行运行 20 秒的短测结果为：

```text
submitted=920
h264=920
mjpeg=890
raw_dropped=30
```

丢帧集中于启动阶段，稳定吞吐约 58～60 FPS。后续还需在 Mac 重新接线后验证实际
USB 连接速度、AVFoundation 协商帧率和长时间显示吞吐。

### 24.4 构建、部署与启动状态

Mac 直接连接 `fx@10.191.32.121`，没有经过树莓派。远端使用 Buildroot/Godot
G++ 13.2.0 交叉编译，`vtb-live2d` 和配置测试目标完成 60 个编译/链接步骤。产物为
ARM64 Linux ELF，`vtb-live2d` SHA-256 为：

```text
7d3da4f4101170b656d96b5ed204c655574e3244762b283d788f4158012c8203
```

部署后通过正常 `reboot` 重建 ConfigFS；没有运行时解绑 UDC。重启后 gadget 和
`vtb-uvc-link.service` 自动启动，Live2D 预览进程与 Link 同时保持运行。Mac USB 线
未连接时 UDC 显示 `not attached`，属于预期状态。
