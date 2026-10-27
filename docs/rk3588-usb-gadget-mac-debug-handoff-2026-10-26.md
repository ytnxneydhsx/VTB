# RK3588 USB Gadget 与 Mac 枚举问题交接记录

> 日期：2026-10-26（Asia/Shanghai）
> 工作目录：`/Users/huangnv/Desktop/VTB`
> 目的：供主 Agent 接手 RK3588 USB Gadget、UVC 视频输出与 Mac 枚举故障排查。
> 本文记录的是已经执行和观察到的事实。计划、假设和未验证事项会单独标明。

## 1. 用户最终目标

用户希望把 RK3588 做成一个 USB 音视频处理设备：

```text
USB 摄像头输入（Host）
  -> V4L2 / MPP / RGA / RKNN(OpenSeeFace) / Live2D / MPP
  -> UVC Gadget 视频输出给电脑

USB 麦克风或声卡输入（Host）
  -> ALSA / 音频模型 / 缓冲与同步
  -> UAC2 Gadget 音频输出给电脑
```

当前阶段只在调通 USB 输出链路；尚未把 OpenSeeFace、Live2D、音频处理和 UVC/UAC2 组成完整产品链路。

## 2. 硬件角色结论

- RK 暴露的两个普通 USB 摄像头接口是 Host 输入口，用于接摄像头、麦克风等外设。
- 面向 Mac/树莓派输出 USB Device 功能的控制器是 `fc000000.usb`，对应 RK 的 OTG/Gadget 口。
- UDC 名称：`fc000000.usb`。
- UDC 报告的最大能力：`super-speed`。
- 实际已观察到的稳定链路速度：USB 2.0 High-Speed，480 Mbit/s。
- 树莓派 USB 拓扑中，RK 接在 Bus 001 的 USB 2.0 Hub 路径；Bus 002 的 5 Gbit/s 总线上没有 RK。

## 3. 已持久化的设备树修改

### 3.1 修改内容

已把 live boot 中的两个 DTB 内以下节点修改为固定设备模式：

```text
/usbdrd3_0/usb@fc000000/dr_mode = "peripheral"
```

刷写前的本地参考 DTB中，该属性为：

```text
dr_mode = "otg";
```

项目中的旧源码补丁文件为：

```text
/Users/huangnv/Desktop/树莓派KVM/ops/rk3588-usb-gadget/
  rk3588-h96-max-v58-usb-peripheral.patch
```

该补丁文本显示的是 `host -> peripheral`，而实际刷写对象中的原值为 `otg`。后续不要仅凭补丁文本推断 live DTB 的原始值。

### 3.2 备份与刷写证据

树莓派上的备份目录：

```text
/home/huangnv/rk3588-usb-peripheral-ss-20261026/
```

该目录保存了：

- GPT 备份
- `dtbo` 备份
- `vbmeta` 备份
- 原 boot 分区备份
- 本次构造与刷写所需的中间产物

处理的 boot 是 48 MiB Android boot v2 镜像。修改了其中两个 DTB，并同步更新：

- Rockchip resource SHA-1
- Android boot v2 ID

只写入 boot 分区，起始 LBA：

```text
0xC800
```

完整读回 SHA-256 与写入镜像一致：

```text
3e78b1da5a60b7022e39aec2a5f430b6695b56e629b6b85b81d7eaccb87fe45c
```

重启后的 live 验证：

```text
/proc/device-tree/usbdrd3_0/usb@fc000000/dr_mode = peripheral
```

### 3.3 设备树是否导致当前故障

现有证据不支持“改成 peripheral 后 RK 就不能工作”：

- 修改并刷写后，树莓派重新枚举了 ACM 与 NCM。
- 树莓派当前仍能稳定识别同一份设备树和 Gadget 配置。
- 树莓派实测 USB NCM ping 为 0% 丢包。
- Mac 路径会反复 reset，而树莓派路径稳定。

因此不要继续盲目修改或回滚设备树。

## 4. 已持久化的 Gadget 服务修改

### 4.1 本地源码

```text
/Users/huangnv/Desktop/树莓派KVM/ops/rk3588-usb-gadget/rk3588-usb-gadget
```

已修改：

```text
bcdUSB=0x0300
max_speed=super-speed
```

还加入了幂等修复：只有首次创建 NCM function 时才写 NCM MAC 地址，避免重复启动服务时报 I/O error。

### 4.2 RK 部署位置

```text
/usr/local/sbin/rk3588-usb-gadget
```

部署前备份：

```text
/usr/local/sbin/rk3588-usb-gadget.before-super-speed-20261026
```

systemd 服务：

```text
rk3588-usb-gadget.service
```

服务单元的 ExecStart：

```text
/usr/local/sbin/rk3588-usb-gadget
```

### 4.3 标准恢复命令

临时实验结束后，使用以下方式恢复标准 ACM + NCM 配置：

```bash
sudo sh -c 'echo "" > /sys/kernel/config/usb_gadget/rk3588_linux/UDC'
sudo rm -f /sys/kernel/config/usb_gadget/rk3588_linux/configs/c.1/uvc.usb0
sudo systemctl restart rk3588-usb-gadget.service
sudo systemctl start serial-getty@ttyGS0.service
```

恢复后的预期值：

```text
product=RK3588 Ubuntu Console
idVendor=0x1d6b
idProduct=0x0104
serial=22b2617583d2c0b7-linux
functions=acm.usb0 + ncm.usb0
service=active
getty=active
```

备注：脚本写入 `bcdUSB=0x0300`，但在实际只协商到 High-Speed 时，configfs runtime 读值曾显示 `0x0210`。不要仅凭 runtime 的 `0x0210` 判定脚本没有部署。

## 5. 树莓派端稳定性验证

### 5.1 2026-10-26 23:34 后的当前对照

把 RK 从 Mac 转接到树莓派后，树莓派识别为：

```text
Bus 001 Device 014: ID 1d6b:0104 Linux Foundation Multifunction Composite Gadget
```

USB 拓扑：

```text
Bus 001 / USB 2.0 / 480M
  -> Hub
     -> NCM control/data
     -> ACM control/data
```

主机驱动：

```text
cdc_ncm
cdc_acm
```

串口：

```text
/dev/ttyACM0
/dev/serial/by-id/
  usb-RK3588_Linux_RK3588_Ubuntu_Console_22b2617583d2c0b7-linux-if02
```

网络：

```text
Pi usb0 = 169.254.4.2/16
RK usb0 = 169.254.4.1/16
carrier = 1
```

短 ping 汇总：

```text
20 packets transmitted, 20 received, 0% packet loss
rtt min/avg/max/mdev = 1.683/1.708/1.886/0.044 ms
```

在持续 ping 和随后静置观察中：

- `/dev/ttyACM0` 持续存在
- `usb0 carrier=1`
- RK UDC 持续 `configured`
- RK `current_speed=high-speed`
- 最后一次插入后的 reset 停在 `23:34:23`
- 之后没有新的 disconnect/unregister

结论：RK 的设备树、DWC3、标准 Gadget 和同一线材在树莓派 Host 上可以稳定工作。

## 6. Mac 端现象与正确检查方法

### 6.1 不要只使用旧 IOUSB plane

以下查询在当前 macOS 上可能只显示控制器，容易误判为“没有设备”：

```bash
ioreg -p IOUSB -l -w 0
system_profiler SPUSBDataType
```

可靠查询应使用 IOService：

```bash
ioreg -p IOService -l -w 0 \
  | rg -i -C 5 'RK3588|Ubuntu Console|USB Camera|idVendor.*7531'
```

ACM/NCM 驱动检查：

```bash
ioreg -r -c AppleUSBACMData -l -w 0
ioreg -r -c AppleUSBNCMData -l -w 0
find /dev -maxdepth 1 \
  \( -name 'cu.usbmodem*' -o -name 'tty.usbmodem*' \) -print
```

### 6.2 Mac 曾经实际枚举成功

Mac 多次短暂出现：

```text
USB Product Name = RK3588 Ubuntu Console
idVendor = 7531 = 0x1d6b
idProduct = 260 = 0x0104
UsbLinkSpeed = 480000000
USBSpeed = 3
```

曾出现的接口：

```text
/dev/cu.usbmodem11301
/dev/tty.usbmodem11301
en7
```

后续重新枚举时，串口后缀变为：

```text
/dev/cu.usbmodem11303
/dev/tty.usbmodem11303
```

串口后缀会随 Mac 重新枚举变化，不能写死。

### 6.3 Mac 当前的实际问题

Mac 不是完全读不到 RK。典型循环是：

```text
Mac 读取全部描述符
-> Set Configuration 成功
-> RK state=configured / high-speed
-> Mac 驱动创建 ACM/NCM 接口
-> 数秒到几十秒后设备消失
-> RK 收到多个 device reset
-> RK state=default / full-speed
-> 再次枚举
```

2026-10-26 23:42 的最新采样：

```text
23:42:41 Mac device=yes，/dev/cu.usbmodem11303 存在
23:42:46 Mac device=no，串口消失
23:42:56 RK configured/high-speed/carrier=1
23:43:01 RK default/full-speed/carrier=0
23:43:06 RK 再次 configured/high-speed/carrier=1
23:43:11 RK configured/high-speed/carrier=0
```

因此“Mac 当前识别到了”只能描述瞬时状态，不能当作稳定性验收。

## 7. Mac/RK 运行时隔离实验矩阵

这些实验均为临时运行时配置，结束后已恢复标准 ACM + NCM。

| 实验 | 变化 | 结果 |
|---|---|---|
| 标准 ACM + NCM | 原 PID `1d6b:0104` | 能短暂枚举，随后 reset |
| ACM-only | 移除 NCM，关闭 `serial-getty@ttyGS0` | 仍 reset |
| ACM-only + 新 PID | `idProduct=0x0105`，新序列号 | 仍 reset |
| ACM-only + Full-Speed | 强制 12 Mbit/s | 仍 reset |
| NCM-only + 新 PID | `idProduct=0x0106` | 仍 reset |
| NCM 持续 IPv6 ping | 保持端点有流量 | 仍周期性 reset |
| NCM 64 MiB 内存传输 | Python socket，不写文件 | 只传约 27 MiB，26.7 秒后超时，约 8.5 Mbit/s |

上述结果排除了：

- NCM 单独导致问题
- ACM 单独导致问题
- 复合设备描述符是唯一原因
- `serial-getty` 持续写端点是唯一原因
- macOS 对旧 PID/序列号的简单缓存是唯一原因
- USB High-Speed 480 Mbit/s 信号协商是唯一原因
- 仅在链路空闲时才发生 reset

## 8. DWC3 trace 结果

开启过以下 tracepoint，捕获后已全部关闭：

```text
dwc3/dwc3_event
dwc3/dwc3_ctrl_req
gadget/usb_gadget_vbus_connect
gadget/usb_gadget_vbus_disconnect
```

跟踪中每次枚举的控制请求均正常完成，包括：

```text
Set Address
Get Device Descriptor
Get String Descriptor
Get BOS Descriptor
Get Configuration Descriptor
Set Configuration(Config = 1)
```

随后典型序列：

```text
Suspend [U3]
WakeUp [U0]
Suspend [U3]
Reset [U0]
Connection Done
```

没有观察到：

- VBUS disconnect
- extcon USB 状态跳变
- EP0 descriptor stall
- Linux runtime PM 真正挂起 DWC3

extcon0 在多轮采样中保持：

```text
USB=1
USB-HOST=0
USB_VBUS_EN=0
SDP=1
```

DWC3 runtime PM：

```text
fc000000.usb/power/control = on
runtime_status = active
runtime_suspended_time = 0
```

父设备 `usbdrd3_0` 虽为 `control=auto`，也没有进入过 runtime suspend。

## 9. `SUSPHY` 设备树假设：目前不要刷

本地保存的 RK 5.10 源码表明驱动支持：

```text
snps,dis_u2_susphy_quirk
rockchip,dis-u2-susphy
```

源码位置：

```text
/Users/huangnv/Desktop/RK3588板卡开发/01-固件备份/Android原机备份/
Linux适配准备/电源-PCB-恢复通道核查/OTG切换源码/core.c

/Users/huangnv/Desktop/RK3588板卡开发/01-固件备份/Android原机备份/
Linux适配准备/电源-PCB-恢复通道核查/OTG切换源码/
phy-rockchip-inno-usb2.c
```

当前 live 寄存器曾读到：

```text
GUSB2PHYCFG(0) = 0x00101448
```

其中 `SUSPHY` 位为 1。

这曾产生“通过设备树禁止 USB2 PHY suspend”的实验假设。但随后完成了树莓派 Host 对照，树莓派稳定。因此当前不应烧写这一 quirk；它最多是未来针对 Mac 特有 suspend/resume 的 workaround 实验，不是已确认修复。

用户已经明确质疑“之前没问题为什么突然改设备树”，主 Agent 应优先保留现有可工作的设备树。

## 10. UVC Gadget 与摄像头验证

### 10.1 摄像头输入

RK 摄像头输入：

```text
/dev/video0
USB path: usb-fc800000.usb-1
```

支持：

- MJPEG 1280x720 @ 30 fps
- YUYV（可用帧率更低）

已完成采集烟雾测试：

```bash
gst-launch-1.0 -m \
  v4l2src device=/dev/video0 num-buffers=60 \
  ! 'image/jpeg,width=1280,height=720,framerate=30/1' \
  ! jpegparse \
  ! fakesink sync=false
```

结果：60 帧成功完成，总耗时约 2.25 秒（含启动开销）。摄像头输入链路正常。

### 10.2 UVC 输出能力

内核已内建：

```text
usb_f_uvc
```

GStreamer 已安装：

```text
/usr/lib/aarch64-linux-gnu/gstreamer-1.0/libgstuvcgadget.so
```

元素：

```text
uvcsink
```

曾临时配置：

- `functions/uvc.usb0`
- MJPEG 1280x720
- frame interval `333333`（约 30 fps）
- UVC-only config
- `high-speed`
- 产品名 `RK3588 USB Camera`

RK 创建成功：

```text
/dev/video2
driver = g_uvc
card = dwc3-gadget
capability = Video Output + Streaming
```

### 10.3 UVC 尚未完成的部分

Mac 没有稳定枚举出 `RK3588 USB Camera`，因此没有启动真正的 GStreamer UVC pass-through 进程。

不要把以下事项说成已经完成：

- Mac 摄像头列表出现 RK UVC
- 真实视频帧通过 USB 到达 Mac
- 10 秒/300 帧 AVFoundation 捕获通过
- 实际 UVC 吞吐或稳定帧率验证通过

理论上的发送命令曾计划为：

```bash
gst-launch-1.0 -e \
  v4l2src device=/dev/video0 io-mode=mmap \
  ! 'image/jpeg,width=1280,height=720,framerate=30/1' \
  ! jpegparse \
  ! queue max-size-buffers=4 leaky=downstream \
  ! uvcsink v4l2sink::device=/dev/video2
```

只有 Mac 已稳定枚举 UVC 后才运行此命令。

## 11. 当前系统状态（交接时）

### 11.1 RK 持久配置

已恢复为标准配置：

```text
product = RK3588 Ubuntu Console
functions = acm.usb0 + ncm.usb0
rk3588-usb-gadget.service = active
serial-getty@ttyGS0.service = active
```

### 11.2 临时实验清理

- UVC function 没有链接到当前 config。
- ACM-only/NCM-only 的临时 PID 已恢复。
- trace `tracing_on=0`。
- 所有开启过的 DWC3/Gadget tracepoint 均为 `enable=0`。
- 本地 TCP 5001 测速监听进程已清理。
- 没有运行 GStreamer `uvcsink` 透传进程。

### 11.3 当前物理连接

用户在树莓派稳定验证后，把 RK 再接到 Mac。最终采样中 Mac 设备短暂出现后再次消失；RK 在 `configured/high-speed` 与 `default/full-speed` 间变化。

用户没有在最后一句中明确说明当前是 Mac 直连还是经过拓展坞，主 Agent 不应自行假设。

## 12. 当前最可靠结论

1. RK 的 `dr_mode=peripheral` 可以正常工作。
2. 标准 ACM/NCM Gadget 在树莓派上稳定。
3. 同一根线在树莓派上稳定，线材不是当前首要嫌疑。
4. Mac 能完整读取描述符并短暂加载 Apple ACM/NCM 驱动。
5. Mac 路径随后主动 reset，导致设备周期性消失。
6. 这个问题不是 UVC 单独导致，因为 ACM-only、NCM-only 也会发生。
7. 当前证据最支持 Mac USB Host Controller、macOS 驱动、端口/Hub 状态或它们与 RK DWC3 的兼容问题。
8. 不应在现阶段继续刷写设备树 quirk。

## 13. 建议主 Agent 的下一步

### 13.1 首选：彻底重置 Mac USB Host 状态

用户当前怀疑 Mac，证据也支持先排查 Mac 路径：

1. 拔掉 RK。
2. 如果使用拓展坞，断开拓展坞与 Mac；有独立供电也断开。
3. 等待至少 30 秒。
4. 重启 Mac，清空 USB Host Controller 和 Apple CDC 驱动状态。
5. Mac 启动后先不要插 RK。

### 13.2 在物理断线时准备干净的 UVC-only

目标是直接测试产品需要的 UVC，避开 CDC ACM/NCM 驱动：

- 使用 UVC-only config
- 使用独立的新 PID 和独立产品名
- 固定 USB 2.0 High-Speed 作为第一阶段
- 配置完成后再插入 Mac
- 不要在带线状态下反复切换功能

建议保留标准 `rk3588-usb-gadget.service` 不变，另做一个明确可恢复的 UVC 测试脚本或 systemd unit；不要在标准恢复脚本中混入临时 UVC 配置。

### 13.3 UVC 验收顺序

1. RK UDC 长时间保持 `configured`。
2. Mac IOService 持续存在 `RK3588 USB Camera`。
3. Mac `SPCameraDataType` 或 AVFoundation 出现摄像头。
4. 启动 `uvcsink` pipeline。
5. Mac 捕获 10 秒或至少 300 帧。
6. 核对帧率、丢帧、断开重连及 RK dmesg。

### 13.4 若重启 Mac 后仍周期 reset

优先收集：

- Mac IOService 设备 sessionID 变化频率
- Mac unified log 中 USB Host 的 reset/disconnect 原因
- RK DWC3 trace 中 reset 前后的最后一个控制请求
- 直连 Mac 与经拓展坞的同一测试结果
- Mac 不同物理端口的结果

到这一步再考虑：

- 针对 Mac 的 `snps,dis_u2_susphy_quirk` 单变量 DTB 实验
- Rockchip USB2 PHY `rockchip,dis-u2-susphy` 单变量实验
- 更新 RK vendor 5.10 DWC3/PHY 驱动

任何 boot DTB 实验都应继续遵守：备份、单变量修改、完整镜像校验、读回校验和可恢复路径。

## 14. 常用访问命令

### 14.1 Mac 通过 RK LAN 访问 RK

```bash
ssh \
  -o StrictHostKeyChecking=yes \
  -o UserKnownHostsFile='/Users/huangnv/Desktop/VTB/network/mac/known_hosts' \
  -o HostKeyAlias=rk3588-ubuntu.local \
  -o IdentitiesOnly=yes \
  -i ~/.ssh/id_ed25519_rk3588_aliyun \
  huangnv@10.201.7.1
```

`rk3588-aliyun` 公网 FRP 路径曾出现 TCP 可连接但 SSH banner 卡住；不要把该路径当作当前可靠管理入口。

### 14.2 树莓派

```bash
ssh rpi4b-aliyun
```

树莓派 alias 当前解析为：

```text
hostname 121.41.75.52
port 7003
user huangnv
identityfile ~/.ssh/id_ed25519_rpi4b
```

### 14.3 RK UDC 快速状态

```bash
cat /sys/class/udc/fc000000.usb/state
cat /sys/class/udc/fc000000.usb/current_speed
cat /sys/class/udc/fc000000.usb/maximum_speed
cat /sys/kernel/config/usb_gadget/rk3588_linux/UDC
find /sys/kernel/config/usb_gadget/rk3588_linux/configs/c.1 \
  -maxdepth 1 -type l -printf '%f -> %l\n'
```

### 14.4 Mac 枚举快速状态

```bash
ioreg -p IOService -l -w 0 \
  | rg -i -C 5 'RK3588|Ubuntu Console|USB Camera|idVendor.*7531'

find /dev -maxdepth 1 \
  \( -name 'cu.usbmodem*' -o -name 'tty.usbmodem*' \) -print
```

## 15. 不要重复或误报的事项

- 不要再次声称“Mac 完全没读到 RK”；它能短暂完整枚举。
- 不要仅凭 RK `state=configured` 宣布成功；Mac 设备节点可能已经消失，UDC 状态也可能短时残留。
- 不要仅查询 `ioreg -p IOUSB` 后断言 Mac 未识别。
- 不要把 `/dev/video2` 的存在当成 UVC 已传到 Mac。
- 不要把摄像头 fakesink 成功当成 USB UVC 透传成功。
- 不要继续修改设备树，除非完成 Mac 重启后的干净 UVC-only 对照并获得新证据。
- 不要在物理连线状态下连续切换 ACM/NCM/UVC 描述符。
- 不要覆盖或删除树莓派上的 boot/GPT 备份目录。
