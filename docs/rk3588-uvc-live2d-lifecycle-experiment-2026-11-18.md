# RK3588 随 Live2D 启停切换 USB 摄像头的实验报告

日期为 2026-11-18。项目基线为 `bbdfe91ff622a105f6c2917a12f75fffccd9c409`。本次从板端只读检查、历史日志和配套内核源码开始。

已在 RK3588 部署随 Live2D 启停切换摄像头的方案。Mac 在 Live2D 运行时识别摄像头并取得 1080p 画面，正常停止和强制结束后均恢复到只有 ACM 串口与 NCM 网卡的状态。隔离实验在同一套 G13 源码上完成了 USB 2.0 和 USB 3.0 各五轮切换。实机这次协商为 USB 2.0 High-Speed，短测中主机采集帧率有明显波动。

## 目标与验收条件

Live2D 运行时，Mac 把 RK3588 识别为 USB 摄像头并能打开视频。Live2D 停止后，Mac 的 USB 摄像头设备消失，ACM 串口和 NCM 网卡在重新枚举后恢复。启停过程中允许三种 USB 功能短暂断开。UVC 由 Live2D 直接提供，取消额外常驻的 `vtb-uvc-link` 进程和 FIFO。板卡继续使用已确认可用于 Rockusb 恢复的 Type-C 口。

## 实验一：板端基线

通过有线 SSH `rk3588-direct` 只读检查。板端内核为 `5.10.110-qem01-linux-probe1`，构建号 `#3`。系统仅注册了 `fc000000.usb` 这一个 USB Device Controller。当前 `rk3588_linux/c.1` 同时挂上了 UVC、ACM 和 NCM。`rk3588-usb-gadget.service` 是已经执行完的 `oneshot`，`vtb-uvc-link.service` 与 `vtb-live2d-camera-preview.service` 都在运行。检查时 UDC 为 `not attached`，因此这轮板端读数不能代替 Mac 枚举及出帧验证。

## 实验二：历史故障与内核源码

此前在运行中解绑 UDC 时，内核在 UVC 清理路径打印调用栈，读取 `UDC` 属性的进程与 `systemd-journald` 都进入不可中断等待状态，正常重启也挂住，最后通过断电恢复。现有的 Bulk 状态修复只覆盖 `VIDIOC_STREAMON` 对连接状态的判断。两者属于不同阶段。 [历史故障记录](rk3588-acm-ncm-uvc-bulk-debug-and-kernel-fix-2026-11-17.md#23-mac-主机切换与运行时-udc-解绑结果) · [已部署的 Bulk 状态补丁](../support/uvc/patches/0001-rockchip-uvc-bulk-streamon-state.patch)

远端找到了当时用于构建板端内核的 G13 源码归档。已核对 `configfs.c` 的写入和读取路径都持有同一个 `gi->lock`。写入端持锁执行完整的设备解绑，因此历史记录里阻塞在 `gadget_dev_desc_UDC_show()` 的进程说明解绑没有返回。这个读端栈本身还不能定位解绑内部卡在哪一步。

已确认旧驱动在应用订阅 `UVC_EVENT_SETUP` 时激活复合设备，并在 UVC 持有者关闭或取消订阅时释放视频队列和停止 USB 连接。既有应用直连 `uvcsink` 的入口可继续用于目标方案。 [Live2D 的两种 UVC 出口](../apps/vtb-live2d/gstreamer_media_pipeline.cpp#L216)

### 从板端 journal 找到另一次故障的完整调用栈

第一次用上游函数名 `uvc_function_unbind` 过滤旧日志时没有找到关键栈。G13 源码注册的回调名是 `uvc_unbind`。按该名称再次查询后，在 2026-11-17 的第三次历史启动记录中定位到 `19:36:10` 的 `uvc_unbind`。`19:36:12` 运行 `uvc_function_bind()`，随即出现 `tried to init an initialized object`。调用链的相关部分如下。

```text
gadget_dev_desc_UDC_store
  → udc_bind_to_driver
  → configfs_composite_bind
  → uvc_function_bind
  → __video_register_device
  → device_register
  → kobject_init
```

后续的设备注册路径又出现一次内核异常。这证明至少有一次故障发生在第二次注册 V4L2 设备时。报告中另一次卡在解绑或清理的事件目前只有当时的记录，尚未从持久日志找回完整栈，两个事件分别保留。

源码解释了第一次故障的对象生命周期。`configfs.c` 的 `purge_configs_funcs()` 在 UDC 解绑时把原有 `usb_function` 移回待绑定列表。若配置链接保持原样，再绑定会把同一个对象用于 `uvc_function_bind()`。旧 G13 驱动把 `video_device` 内嵌在 `uvc_device` 中，并在 `uvc_register_video()` 调用 `video_register_device(&uvc->vdev, …)`。单纯解绑不会把这块内存变成新的设备对象。相比之下，解绑后移除配置中的函数链接会调用 `usb_put_function()`，重新建立链接会调用 `usb_get_function()`，UVC 实现会重新分配一个 `uvc_device`。这给出了避免重用原 V4L2 对象的办法。 [Linux V4L2 设备注册与释放](https://docs.kernel.org/driver-api/media/v4l2-dev.html)

也检查了上游 UVC 应用清理提交 `b81ac4395bbe`。归档内核已存在该方案的两阶段等待。新的清理改动可以作为后续板端排查的来源，目前没有证据能将其直接等同于这次重新注册的故障。 [上游应用清理提交](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=b81ac4395bbeaf36e078dea1a48c02dd97b76235)

## 实验三：G13 源码和虚拟 USB 控制器

实验只验证安全的候选路径。每次先等 UVC 持有者完全退出，再解绑 UDC，拆除配置中的函数链接，按 UVC、ACM、NCM 的顺序新建所需链接后重新绑定。无摄像头状态只创建 ACM 和 NCM 链接。UVC 的格式及帧率参数保留在 ConfigFS 的函数实例目录中，新的链接分配新的 `usb_function`。保留 UVC 优先的次序与既有板端脚本一致。 [板端端点分配顺序](../support/uvc/start-rk3588-acm-ncm-uvc-bulk-gadget.sh#L105)

编译服务器采用与板端配套的 G13 源码构建一个 x86_64 测试内核。独立启用 `dummy_hcd`，在无外部网络的 QEMU 中同时模拟 USB Host 和 Gadget。物理板的内核及 UDC 无需参与。服务器现有发行版内核缺少 `dummy_hcd`，且测试账户无法读取 `/boot/vmlinuz`，因此构建实验专用内核。第一次默认调用了厂商 GCC 包装器，Kconfig 没检测到有效编译器，构建报 `Compiler lacks asm-goto support.`。单独执行 GCC 9 的探测成功，重试显式使用 `/usr/bin/gcc-9`，构建全过程在 `/tmp/vtb-uvc-lifecycle-20261118.lgHM4S` 的独立输出目录执行。

### 实验结果

在隔离环境中，复用了项目的 Bulk Gadget 描述符脚本。测试程序打开 V4L2 节点并订阅 `UVC_EVENT_SETUP`，模拟摄像头进程持有设备的行为。每轮从 `ACM + NCM` 切到带 UVC 的配置，在确认测试持有者退出后切回。模拟的 USB Host 从 sysfs 检查真实枚举出的接口类，日志同时显示两个模式下 `cdc_acm` 和 `cdc_ncm` 的驱动注册。测试程序首次编译缺少 `<sys/ioctl.h>`，补齐头文件后以 `-Wall -Wextra -Werror` 构建通过。正式验证使用 QEMU 文件串口记录内核和脚本输出，并独立检查进程退出码与结束标记。

| 条件 | 结果 |
| --- | --- |
| USB 2.0 High-Speed | 五轮通过，其中第三轮以 `SIGKILL` 结束持有者，随后切回通过 |
| USB 3.0 SuperSpeed | 五轮通过，其中第三轮以 `SIGKILL` 结束持有者，随后切回通过 |
| 带 UVC 的模式 | Host 出现两个 `0e` 类视频接口，`cdc_acm` 与 `cdc_ncm` 一同注册 |
| 仅 ACM 与 NCM 的模式 | Host 的视频接口消失，两个通信驱动在重新枚举后注册，Gadget 的 `/dev/video0` 消失 |
| 内核检查 | 两次正式实验均以退出码 0 结束，日志中未出现历史的重复初始化告警、Oops 或 panic |

证据文件包括 [历史调用栈摘录](../build/remote-artifacts/rk3588-uvc-lifecycle-20261118/board-historical-rebind-excerpt.txt)、[USB 2.0 串口日志](../build/remote-artifacts/rk3588-uvc-lifecycle-20261118/qemu-usb2.log)、[USB 3.0 串口日志](../build/remote-artifacts/rk3588-uvc-lifecycle-20261118/qemu-usb3.log)、[编译与实验清单](../build/remote-artifacts/rk3588-uvc-lifecycle-20261118/manifest.txt)和[校验文件](../build/remote-artifacts/rk3588-uvc-lifecycle-20261118/SHA256SUMS-evidence)。实验用 [Init 脚本](../build/remote-artifacts/rk3588-uvc-lifecycle-20261118/init.sh)和 [V4L2 持有者](../build/remote-artifacts/rk3588-uvc-lifecycle-20261118/uvc-owner.c)随日志保留。

## 实机启停顺序

板卡开机只配置 `ACM + NCM`。在可靠的热切换条件下，Live2D 服务启动前切到 `ACM + NCM + UVC`，由 Live2D 的 `uvcsink` 打开 Gadget 视频节点。Live2D 正常退出或异常退出后，由服务清理步骤切回 `ACM + NCM`。两个配置的切换都由短时命令完成。 [Linux Gadget ConfigFS](https://docs.kernel.org/usb/gadget_configfs.html) · [systemd 服务启停语义](https://www.freedesktop.org/software/systemd/man/latest/systemd.service.html#ExecStopPost=)

## 实机接线和数据枚举

QEMU 使用 x86_64 的 `dummy_hcd`，覆盖了 G13 的 ConfigFS、USB 复合设备和 UVC/V4L2 对象生命周期。RK3588 DWC3 端点释放、Mac 枚举以及 Live2D 实际出帧在物理板上单独验证。

实机验证首先使用 Type-C 连接 Mac，有线 SSH 独立观察内核日志。一次验证先测正常启停，确认 `uvcsink` 退出后再切换。Mac 端在启动状态确认摄像头出现并能获取视频，在停止状态确认摄像头消失且 ACM/NCM 恢复。通过后再加异常退出验证。由于之前曾在解绑时卡死，实机试验需要现场能在必要时断电恢复。

### 第一次接线检查

用户第一次接线后，Mac 的 USB 设备树和摄像头列表均没有 RK。板端 `vtb-uvc-link` 和 Live2D 仍在运行，配置含有三个函数。两次读取 `fc000000.usb` 均为 `not attached`、`UNKNOWN`。同时 OTG PHY 的 Extcon 为 `USB=1`、`SDP=0`、`DCP=1`。配套 PHY 源码将 DCP 定义为专用充电端口。此时板端已感知连接并将其分类为充电端口，数据枚举尚未完成。

用户确认该线可传数据。复查时 RK 的 UDC 仍为 `not attached`、`UNKNOWN`，控制器的 runtime PM 为 `active`。Mac 的 IOService 未出现 RK 复合设备或 USB modem 串口。历史上同一板卡的 OTG 口已经在 Mac 上枚举并完成摄像头取帧。 [此前 Mac 验证记录](rk3588-acm-ncm-uvc-bulk-debug-and-kernel-fix-2026-11-17.md#23-mac-主机切换与运行时-udc-解绑结果)

### 拔插后重新检查

2026-11-18 21:50（北京时间），RK 的 UDC 报 `configured`、`high-speed`。OTG PHY 变为 `CDP=1`、`DCP=0`，内核记录了 `device reset`。Mac 的 USB 设备树出现 `RK3588 Live2D Composite`，有两项视频类接口，系统摄像头列表出现 `UVC Camera`。ACM 串口是 `/dev/cu.usbmodem1103`，NCM 网卡是 `en7`，板端对应 `usb0` 已起来。

这期间板子也发生了重启，恢复的具体触发因素未确定。当时 `vtb-uvc-link` 在运行，Live2D 预览服务已停，确认常驻 Link 能让 Mac 在 Live2D 停止时仍枚举摄像头。

## 实机热切换和部署

通过独立有线 SSH 操作，先停止旧 Link 并确认它释放 `/dev/video2`。试验脚本成功将配置切为 ACM + NCM，Mac 上的摄像头消失。随后加回 UVC，启动现有 Live2D 可执行文件并使用省去 `output.uvc.link_path` 的专用配置。应用的 `uvcsink` 直接持有 `/dev/video2`。Mac 通过 AVFoundation 取得 30 帧 1920×1080 图像，并用亮度统计验证缓冲区含有画面数据。这次临时启停结束后，Mac 又恢复到 ACM + NCM。 [首次切换日志](../build/remote-artifacts/rk3588-uvc-lifecycle-20261118/board-first-switch-base.log) · [首次捕获日志](../build/remote-artifacts/rk3588-uvc-lifecycle-20261118/mac-direct-capture-60.log)

板端将按需模式固化到 systemd。开机脚本创建 UVC 描述符，但仅将 ACM 和 NCM 加入 USB 配置。`vtb-live2d.service` 由用户按需启动。它在主进程启动前执行 `vtb-uvc-switch camera`，在主进程结束后执行 `vtb-uvc-switch base`。脚本先确认没有进程持有 UVC 视频节点，再解绑控制器并重建 ConfigFS 配置链接。应用继续使用已部署的 Live2D 二进制，专用运行配置位于 `/etc/vtb/live2d-uvc.json`，可执行文件和运行库路径来自 `/etc/vtb/live2d-uvc.env`。旧 Link 服务被停止并禁用。 [切换脚本](../support/uvc/switch-rk3588-uvc-gadget-mode.sh) · [开机配置](../support/uvc/20-vtb-live2d-uvc-on-demand.conf) · [Live2D 服务](../support/uvc/vtb-live2d.service) · [安装文件校验](../build/remote-artifacts/rk3588-uvc-lifecycle-20261118/SHA256SUMS-install)

| 实机检查 | 观测结果 |
| --- | --- |
| 启动已安装服务 | Mac 出现 `UVC Camera`，USB 设备树显示两个视频接口和 ACM/NCM。AVFoundation 取得 60 帧 1920×1080 图像，用时约 5.1 秒。 [捕获日志](../build/remote-artifacts/rk3588-uvc-lifecycle-20261118/mac-service-capture-60.log) |
| 正常停止 | systemd 退出状态成功，Mac 的视频接口消失，`/dev/cu.usbmodem1101` 重新出现，NCM `en7` 到板端 IPv6 地址的两次 ping 均成功。 [板端日志](../build/remote-artifacts/rk3588-uvc-lifecycle-20261118/board-service-graceful-stop.log) · [Mac 复核](../build/remote-artifacts/rk3588-uvc-lifecycle-20261118/mac-service-graceful-stop.log) |
| 强制结束主进程 | 用 SIGKILL 验证异常退出。systemd 报主进程被信号结束，`ExecStopPost` 返回 0。Mac 上 UVC 消失，ACM 串口与 NCM 网卡恢复，NCM 两次 ping 均成功。板端出现 DWC3 端点请求清理日志，随后完成 UDC 绑定和主机 reset。 [板端日志](../build/remote-artifacts/rk3588-uvc-lifecycle-20261118/board-service-sigkill.log) · [Mac 复核](../build/remote-artifacts/rk3588-uvc-lifecycle-20261118/mac-service-sigkill.log) |
| 正常重启板端 | boot ID 改变。开机服务只将 ACM 与 NCM 加入配置，Mac 初次枚举没有摄像头，串口出现且 NCM 两次 ping 均成功。Live2D 和旧 Link 均未自动启动。 [板端日志](../build/remote-artifacts/rk3588-uvc-lifecycle-20261118/board-reboot-base.log) · [Mac 复核](../build/remote-artifacts/rk3588-uvc-lifecycle-20261118/mac-reboot-base.log) |
| 重启后再次启停 | Mac 再次识别摄像头，15 秒内采得 308 帧 1920×1080 图像。停服务后 Mac 的 UVC 消失、串口恢复、NCM 两次 ping 均成功。板端当次启动的内核日志未匹配历史的重复初始化、Oops 或 panic。 [捕获日志](../build/remote-artifacts/rk3588-uvc-lifecycle-20261118/mac-reboot-capture.log) · [板端最终状态](../build/remote-artifacts/rk3588-uvc-lifecycle-20261118/board-final-base.log) · [Mac 最终状态](../build/remote-artifacts/rk3588-uvc-lifecycle-20261118/mac-final-base.log) |

Mac 捕获均请求了 USB 描述符宣告的 60 fps。已安装服务第一次短测实收平均约为 12 fps。重启后的 15 秒样本平均约 21 fps，开流初段有间歇性停顿，最后约 3.4 秒接近 60 fps。停顿原因和长时间稳定帧率未确认。USB 重新枚举时串口设备名从 `cu.usbmodem1103` 变为 `cu.usbmodem1101`，已有的串口连接需要重新打开。NCM 在重启后的摄像头枚举过程中出现一次 IPv6 ping 丢包，进入最终停止状态后再次通过两次 ping。

## 交付状态和恢复方式

板子最后留在 Live2D 停止状态。日常从 RK 上启动和停止同一个服务即可，应用内的 `uvcsink` 提供视频，启停由 systemd 调用短时切换脚本。

```bash
sudo systemctl start vtb-live2d.service
sudo systemctl stop vtb-live2d.service
```

本次部署使用现有板端内核、设备树和 Live2D 二进制。旧版 Gadget 启动脚本、原 systemd drop-in、旧 Link 服务和它使用的配置均保留。若需要恢复常驻摄像头配置，先停止按需服务，撤下新增的 `20-vtb-live2d-uvc-on-demand.conf`，重新加载 systemd、启用旧 Link 并正常重启，让板子按原模式重新创建 USB 配置。

## 直连 UVC 的 10 帧 JPEG 队列

后续测试发现，开启 `v4l2sink` 的时钟同步时，部分 JPEG 在提交给 USB 前已经被判迟丢弃。板端跟踪中，发生卡顿的一段约 618 ms 内判迟丢了 35 帧。成功提交的帧从 V4L2 入队到 USB 完成传输通常约 2.4 ms。对照实验为 UVC 单独补偿 40 ms 后，`v4l2sink` 的判迟日志归零。两帧的编码后队列下 Mac 实收约 41 fps，六帧队列复测约 58.5 fps。时钟补偿为帧预留交付时间，编码后队列为等待交付的帧提供存放空间。

依照用户要求，正式版在直连时使用 `jpegparse → queue → uvcsink`，JPEG 队列最多缓存 10 帧，满时丢最旧的帧。`uvcsink` 内的 `v4l2sink` 使用 `sync=true` 和 `ts-offset=40000000`。60 fps 目标与现有 PTS 计算沿用原配置。部署包以已运行二进制匹配的板端源码归档为基础，只应用本次 UVC 修改。当前工作区版本单独完成 ARM64 编译。 [直连分支](../apps/vtb-live2d/gstreamer_media_pipeline.cpp#L244) · [构建和安装清单](../build/remote-artifacts/rk3588-uvc10-20261118/manifest.txt)

正式 `vtb-live2d.service` 启动新版后，Mac 在 32 秒内取得 1,884 帧 1080p 视频，平均 58.9 fps，帧间隔中位数 16.9 ms，P95 为 18.0 ms。开流时出现一次约 209 ms 的间隔，其余间隔最大约 34.3 ms。RTSP 在 Mac 打开和关闭 USB 摄像头前后均持续出帧，44 秒内解码 2,641 帧。停止服务后摄像头消失，ACM 串口重新出现，NCM 的 IPv6 ping 两次均成功。 [采集数据](../build/remote-artifacts/rk3588-uvc10-20261118/capture-summary.txt) · [Mac 停机复核](../build/remote-artifacts/rk3588-uvc10-20261118/mac-camera-off.txt)

板端按需服务仍处于停止状态，下次启动会使用新版程序。上个版本及服务环境备份 `/etc/vtb/live2d-uvc.env.before-uvc10-20261118` 保留在板上。
