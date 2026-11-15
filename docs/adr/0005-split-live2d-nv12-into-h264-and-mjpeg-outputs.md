# ADR 0005：从同一 NV12 帧分出 H.264 网络流和 MJPEG UVC 流

## 状态

已实现双编码和双输出。ABI v4 的模块内 MPP 版本完成过 RK3588 版本化部署：板端验证了
GStreamer 1.28.9 插件、`/dev/video2` 的 UVC Video Output + Streaming 能力和双 MPP
持续编码；Mac 完成 RTSP 连续解码；树莓派 USB 主机完成 MJPEG 1920×1080@30 枚举、
连续抓帧、关闭后重新打开和与 RTSP 同时取流。ABI v6 已把 RGA 与 MPP 都移到宿主的
`RgaNv12FrameTransformer`、`MediaEncodeSession`，需要重新完成 ARM64 构建与板端回归后才能
继承运行验收结论。

## 背景

Live2D 渲染链已经在 Godot 所有者线程内完成 GPU 复制和一次 RGA RGBA→NV12 转换。
RTSP 输出需要 H.264。Linux UVC Gadget 的 GStreamer `uvcsink` 路径接受已经编码的
MJPEG 或未压缩 YUYV；当前选用 MJPEG，以控制 USB 带宽和 CPU 拷贝量。

把 H.264 包直接交给 `uvcsink` 无法满足当前插件的输入协商。把 H.264 解码后再编码
MJPEG也会增加一次无意义的解码。因此使用两个独立 MPP 编码会话，共享同一张只读
NV12 DMA-BUF，并在同一所有者线程内顺序调用。

## 决策

```text
Godot / GPU
    ↓ RGBA DMA-BUF
RGA（每帧一次 RGBA → NV12）
    ↓ 同一张只读 NV12 DMA-BUF
    ├─ MPP H.264（渲染帧率）→ OutputManager → FFmpeg RTSP
    └─ MPP MJPEG（默认 30 fps）→ OutputManager → GStreamer 1.28.9 uvcsink → UVC Gadget
```

- 两个 MPP 会话分别拥有自己的 `MppCtx`、配置、输入 DMA-BUF 导入和输出包租约。
- RGA 业务适配属于宿主 `RgaNv12FrameTransformer`，实际命令由共享 Executor 的 Worker
  执行；H.264 MPP 和 MJPEG MPP 属于宿主 `MediaEncodeSession`。LibGodot 只交付 GPU 已完成
  写入的 RGBA DMA-BUF。Godot 所有者线程同步等待 RGA future，再顺序调用两个 MPP；没有
  增加 NV12 帧队列或编码工作线程。
- ABI v6 先通过 `RgbaFrameSink` 借用 RGBA DMA-BUF，再由宿主通过 `Nv12FrameSink`
  借用转换结果；宿主 `EncodedVideoSink` 用
  `EncodedVideoCodec` 区分 H.264 和 MJPEG，并按编码格式
  注册两个流并绑定到两个独立输出工作队列。
- UVC 分支按整数时间表从渲染帧率降采样。例如 60 fps 渲染、30 fps UVC 时每两帧编码
  一张 MJPEG，不复制或缓存第二张 NV12 图像。
- GStreamer 只负责 MJPEG 解析、UVC 协商和 V4L2 Output 写出，不参与 MPP 编码。
- GStreamer 1.28.9 以 `/opt/vtb/gstreamer-1.28.9` 为前缀并行安装；应用通过显式的
  `gst-launch-1.0`、库目录和插件目录选择该版本，不覆盖板端发行版的 1.24.2。
- 板端 5.10 gadget 节点不实现 `VIDIOC_ENUM_FMT`。项目构建为 `uvcsink` 应用兼容
  补丁，直接从 ConfigFS 读取 MJPEG、1920×1080 和 30 fps 描述完成 caps 协商。
- ACM+UVC 复合设备的 UVC 接口编号为 2/3。兼容补丁使用 ConfigFS 中的实际接口
  编号，按主机 `wLength` 响应 UVC 1.0 的 26 字节 Probe/Commit 控制。
- `v4l2sink` 的兼容路径只在 `g_uvc` Video Output 返回 `ENOTTY` 时补充 MJPEG 格式，
  其他 V4L2 设备继续使用原始枚举逻辑。

## 代价与边界

- 每个 UVC 帧增加一次 MPP MJPEG 编码，占用第二个编码上下文和额外内存带宽。
- 两次 MPP 调用目前串行执行。是否拆成独立线程由 `live2d.mpp_h264`、
  `live2d.mpp_mjpeg` 和总帧周期的实测结果决定。
- UVC 输出阻塞被限制在 `OutputManager` 的 UVC 工作线程和有界队列内；它不会在
  `uvcsink` 写入阻塞时直接卡住 Godot 所有者线程。
- UVC Gadget 描述符、UDC 绑定和 Mac 枚举属于板端部署阶段。完成交叉编译不代表 USB
  摄像头已经在 Mac 上可用。
- 当前 BSP 的 ACM+NCM 组合无法再分配 UVC streaming endpoint。UVC 模式保留 ACM，
  暂时从配置中取消 NCM 链接。
- 当前 Linux 5.10.110 BSP 重复解绑并绑定同一个 UVC function 会在
  `uvc_function_bind()`/`uvc_unbind()` 路径触发内核异常。配置脚本对已激活实例直接
  返回；解绑后需要重启并重新创建 function。
- 该 BSP 在绑定 UDC 前不创建 `/dev/video2`，同时不支持 UDC `soft_connect`。
  `uvcsink` 无法在主机第一次枚举前预先打开节点。启动应用后需要连接或重新连接
  一次 USB 主机，使 UVC `GET_CUR(PROBE)` 请求由已运行的 userspace 响应器处理。

## 验证要求

以下 1～8 项是 ABI v4 路径的历史验收记录。ABI v6 必须重新执行相同验证，并增加
LibGodot 不再链接 `librga`、`librockchip_mpp`，宿主直接链接两者的 ELF 依赖检查。

1. 在 RK3588 上确认两个 MPP 会话能够同时初始化并持续编码。
2. 已确认 GStreamer 1.28.9 的 `coreelements`、`jpegparse`、`v4l2sink` 和 `uvcsink` 插件可加载。
3. 已确认 `/dev/video2` UVC Gadget 节点具有 Video Output + Streaming 能力。
4. 已验证一轮 14480 帧 H.264、7240 帧 MJPEG：两个输出队列均为零丢帧、零错误；
   媒体总吞吐约 57.1 fps。
5. 已在 Mac 通过 RTSP/TCP 连续解码 5 秒，识别为 H.264 1920×1080、约 60 fps。
6. 已在树莓派主机枚举为 MJPEG 1920×1080@30；第一次抓取 30 帧，关闭后第二次
   抓取 150 帧，两次均由 FFprobe 读出全部帧，第二次的 150 帧由 FFmpeg 完整解码。
7. 第二次 UVC 抓帧期间，Mac 同时通过 RTSP/TCP 解码 H.264 五秒，两条输出分支均
   正常完成。
8. 最终正常模式进程持续运行超过 100 秒，并再次通过 30 帧 UVC 与三秒 RTSP 并发
   烟雾测试；运行期间未再触发 RKNN 检测置信度越界终止。
