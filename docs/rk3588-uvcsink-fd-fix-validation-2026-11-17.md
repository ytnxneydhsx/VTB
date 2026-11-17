# RK3588 上 uvcsink 覆盖 DMA-BUF fd 的复现与验证

2026-11-17 在 RK3588（AArch64，Linux `5.10.110-qem01-linux-probe1`）上验证了 GStreamer 1.28.9 的 `uvcsink` 修复。板端使用 ConfigFS UVC Gadget，设备节点为 `/dev/video2`。

## 启动复现

Live2D 在 GStreamer pipeline 进入 `PLAYING` 前分配四个 RGA NV12 DMA-BUF 槽位。NV12 经 `appsrc` 和 `tee` 送往两路编码，MJPEG 分支的末端为 `jpegparse → uvcsink`。从进程启动前用 `strace -f -yy` 跟踪 fd 操作。以下为去掉时间戳及无关调用后的记录。

```text
openat(AT_FDCWD, "/dev/video2", O_RDWR) = 52</dev/video2>
dup3(52</dev/video2>, 53</dmabuf>, O_CLOEXEC) = 53</dev/video2>
fcntl(53</dev/video2>, F_DUPFD_CLOEXEC, 0) = 61</dev/video2>
```

`gst_uvc_sink_watch()` 用 `device_fd + 1` 作为 `dup3()` 的固定目标，关闭并替换了已经归 NV12 池使用的 fd 53。RGA 已导入的 handle 仍保活原缓冲区，应用保存的 fd 编号却已经指向 UVC 设备。两路编码分支随后收到错误的描述符，内核记录了 `dma_buf_get fail` 和 `mm import buffer failed`。

## 修复

[补丁 0006](../support/gstreamer/patches/0006-uvcsink-allocate-watch-fd.patch) 将 `dup3(device_fd, device_fd + 1, O_CLOEXEC)` 改为 `fcntl(device_fd, F_DUPFD_CLOEXEC, 0)`，由内核分配空闲描述符并设置 close-on-exec。构建脚本通过现有 `000*.patch` 规则应用该补丁。

## 板端验证

- 使用 Buildroot/Godot GCC 13.2.0 交叉编译 GStreamer 1.28.9 的 UVC 插件和验证用 Rockchip MPP 插件。AArch64 产物在取回和部署前均通过 SHA-256 校验。
- 启动跟踪显示 `fcntl(52</dev/video2>, F_DUPFD_CLOEXEC, 0) = 57</dev/video2>`，NV12 池的 fd 53–56 仍指向四块 DMA-BUF。
- 验证使用 GStreamer MPP 插件版的 Live2D 链路。移除临时强制拷贝逻辑后，编码日志记录 H.264 **4,884 帧**、MJPEG **2,446 帧**走直接导入，两路 RGA 和 CPU 拷贝回退均为 **0 次**。
- 恢复常规运行后，Mac 通过 RTSP 解码 301 帧 1920×1080@60 H.264。板端另取 10 秒观察窗口，未出现新增 RGA 或 MPP 内核错误。

本次 MJPEG 验证覆盖板端编码输出。Mac USB 摄像头枚举和抓帧仍待验证。
