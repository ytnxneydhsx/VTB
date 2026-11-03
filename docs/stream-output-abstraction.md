# 公共媒体输出层设计

## 为什么需要这一层

当前仓库的网络发布由外部 FFmpeg 与 MediaMTX 脚本完成，UVC 仍处于单段实验阶段，UAC2 尚未接入。最终产品需要把同一处理结果同时送往网络和 USB，并允许单个出口断开或变慢时继续运行其他出口。

公共输出层解决以下问题：

- 处理链只提交一次媒体数据，不逐个调用具体协议；
- 每个出口独立排队和发送，慢出口不占用生产线程；
- 出口可以在运行期间独立启动、停止、绑定和移除；
- 每个出口提供统一状态、队列和错误统计。

这一层不负责以下工作：

- 视频或音频编解码；
- 像素格式、采样格式或采样率转换；
- USB Gadget configfs 的创建、绑定或枚举；
- 修复上游时间戳、音视频漂移或缺失的关键帧；
- 自动选择编码格式。

## 更简单的方案及选择理由

更简单的方案是让处理链直接调用固定的 RTSP、UVC 和 UAC2 发送函数。它适合只有一个固定出口的阶段，但会把协议生命周期、阻塞和错误处理带进生产线程。当前需求明确包含多个出口动态组合、运行时启停和故障隔离，因此保留一个最小的 `OutputManager`。

首版不引入动态库插件、HTTP 控制、配置热重载或通用事件总线。出口类型在编译期确定，通过启动参数和交互式命令组合。

## 数据路径

```text
已经编码或格式化的媒体数据
             |
             v
        OutputManager
          /   |   \
         /    |    \
   RTSP 队列 UVC 队列 UAC2 队列
       |        |        |
   RTSP worker UVC worker UAC2 worker
       |        |        |
   MediaMTX   V4L2 Out  ALSA PCM
```

`publish()` 只校验、按出口取得共享缓冲区引用并入队。分叉时每个出口获得一份
`shared_ptr` 引用；进入某个出口后，`MediaPacket` 从队列移动到工作线程，再移动到
具体后端。网络或设备写入只在对应出口的工作线程中执行。

## 媒体契约

流在发送数据前注册。`StreamDescriptor` 描述固定格式，`MediaPacket` 引用流 ID 并携带单调时钟时间戳。

视频描述至少包含编码或像素格式、宽、高和帧率；音频描述至少包含采样格式、采样率和声道数。编码器配置数据属于描述符。格式发生变化时使用新的流 ID 和递增版本注册描述符，再重新绑定并启动受影响出口；同一流 ID 不会被原地覆盖。

数据负载为不可变共享缓冲区。上游允许将结果复制一次进入共享缓冲区；多个出口共享
同一份字节数据。移动 `MediaPacket` 只转移该引用和元数据，不复制 payload。

当前 Live2D 硬件编码路径中，MPP packet 由媒体线程的 RAII 租约持有，并通过
`EncodedVideoPacketView` 同步借给 `MediaOutputSession`。会话在回调返回前完成唯一一次
MPP packet 到共享 payload 的字节复制；回调返回后才释放 MPP packet。这个边界使编码器
缓冲区寿命与异步输出队列解耦，也删除了编码模块内部的临时 `vector`。

RTSP 分支使用只读 `AVBufferRef` 包装共享 payload 的地址，释放回调持有从
`MediaPacket` 移入的 `shared_ptr`。因此 `MediaPacket` 到 `AVPacket` 不再复制字节；
FFmpeg 最后释放 `AVBufferRef` 时才释放该分支的 payload 引用。UVC 当前仍复制到 V4L2
MMAP buffer，DMA-BUF 输出属于后续依赖驱动能力的阶段。


## 出口与格式匹配

- `RtspOutput` 可以绑定一路视频和一路音频，使用 libavformat 完成复用及 RTSP/RTP 发送；
- `UvcOutput` 绑定一路视频，通过 V4L2 Video Output 发送当前 Gadget 已声明的格式；
- `Uac2Output` 绑定一路 PCM 音频，通过 ALSA 写入 Gadget 设备。

每个出口在启动时验证描述符。格式不兼容时拒绝启动并报告原因。出口不会转码。

## 生命周期与故障边界

出口生命周期为 `stopped -> starting -> running -> reconnecting -> running`；达到显式重试上限时进入 `failed`，人工停止后回到 `stopped`。管理器提供新增、绑定、启动、停止、解绑、移除、查询和发布操作。

每个出口拥有独立有界队列和工作线程。实时队列满时丢弃最旧数据并记录 discontinuity；视频出口在丢弃压缩预测帧后等待新的关键帧。出口异常只改变本出口状态，其他出口继续运行。网络或设备重连使用有界退避，停止操作可以中断等待。

实时出口停止时停止接收新包并丢弃队列。未来文件出口可以实现带超时的排空策略。

## 命令行边界

公共输出层以进程内 C++ 库形式提供，调用方直接管理流、出口和 `publish()`。交互式命令行是默认关闭的诊断示例，可由命令注册流、创建出口并执行 `list`、`start`、`stop`、`bind`、`unbind`、`remove` 和 `quit`。HTTP、Unix Socket 控制、启动配置文件和配置自动保存留到出现实际调用方后再实现。

## 真实编码包入口

`vtb-media-output-publisher` 是公共输出层的首个真实媒体包生产者。它从非 seekable 的 FFmpeg 输入读取 H.264 包，取得 SPS/PPS、时间戳和关键帧标记，映射到单调时钟纳秒后调用 `OutputManager::publish()`。Face3D 链路使用 MPEG-TS 管道保留编码包边界和时间戳，FFmpeg 仅负责编码，RTSP 连接、队列、重连和发送全部由 `RtspOutput` 完成。

`RtspOutput` 的 I/O 超时由 FFmpeg interrupt callback 和单调时钟 deadline 控制。RTSP muxer 参数只传入它实际消费的 `rtsp_transport`；不再重复传入部分 FFmpeg 版本不会消费的 `rw_timeout`，避免连接已经建立后被误判为未知输出选项。

更简单的旧路径是让 FFmpeg CLI 直接发布 RTSP。它可以显示画面，但会绕过公共输出层，无法验证多出口共用的队列、故障隔离和状态统计。当前入口保留一个额外本地进程和一次编码包复制，首阶段以接口真实性和可观测性为目标；后续只有在测量证明进程边界或复制成为瓶颈时，才把编码器直接嵌入 C++ 生产链。

验收至少同时满足：`RtspOutput` 进入 running、`written` 持续增加且无错误、MediaMTX 接收到 `face3d` 发布流、Mac 浏览器持续解码 WebRTC 视频帧。仅监听端口或打开 WebRTC 页面不足以证明媒体链成功。

### RK3588 直接硬件编码入口

`vtb-mpp-output-publisher` 用于 RK3588 实时 Face3D 链路。它从标准输入读取连续 BGR24 帧，将 CPU 写入的 BGR DMA-BUF 通过 RGA 同步转换成 NV12 DMA-BUF，再由 MPP 输出 H.264 Annex B 包，并直接构造 `MediaPacket` 调用 `OutputManager::publish()`。MPP 提供的 SPS/PPS用于注册 `StreamDescriptor`，IDR NAL用于关键帧标记。

这条路径移除了 FFmpeg 编码进程、MPEG-TS Pipe、容器封装和 C++ 解复用；保留 Python/RKNN 到 C++ 的 BGR Pipe，因此仍有原始帧的跨进程复制。首版沿用已验证探针的单缓冲同步调用，先验证正确性并记录 RGA、MPP 和输出队列耗时；多缓冲和进一步减少 BGR 搬运只在测量后实施。

当前 BSP 将 `/dev/mpp_service` 和 DMA heap 设为 root-only。测试脚本只在普通用户无法访问这些节点时，通过已有的无交互 sudo 启动 `vtb-mpp-output-publisher`；Python、MediaMTX 和监督脚本仍以普通用户运行。长期部署需要单独审查并配置最小 udev 用户组权限。

## 分阶段验证

### 里程碑 C：单出口正确性

- 公共媒体类型、生命周期和格式校验通过静态检查与单元测试；
- 假出口验证 fan-out、队列上限、停止和故障隔离；
- RTSP、UVC、UAC2 分别完成单段发送；
- 物理 USB 测试在执行前取得单独授权。

### 里程碑 D：并行与背压

- 至少两个真实出口同时消费同一上游；
- 人为减慢或断开一个出口时，其他出口持续运行；
- 记录每个出口的输入、发送、丢弃、错误、队列深度、队列驻留时间和写入耗时；
- 记录 PTS 单调性、音频 underrun/overrun、视频关键帧恢复、CPU、内存和复制次数；
- 长时间运行验证无死锁、泄漏或无界内存增长。

## 已知前置条件

- 当前 C++ 主链还没有媒体包生产者，首版接口需要通过假数据源和后续真实编码链逐步校准；
- 远端编译服务器当前未发现 ARM64 交叉编译器、目标 sysroot、FFmpeg 或 ALSA 目标开发文件；
- UVC Host 枚举和真实帧传输尚未完成，UAC2 链路尚未建立。

这些条件不会通过在 Mac 或 RK3588 本地编译来绕过，也不会在没有确认的情况下安装、降级或替换依赖。
