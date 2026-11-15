# ADR 0004：在一个所有者线程同步串联渲染与编码模块

状态：已由 ADR 0005 取代；本文保留同步编码基线的设计背景
日期：2026-11-14

当前代码把 NV12 Lease 移交给 GStreamer，并在两个分支流线程分别执行 H.264 与
MJPEG MPP 编码。

## 背景

原 `OpenVTDmaBufPipeline` 在 Godot 所有者兼渲染线程完成 GPU copy，再把三个 RGBA
DMA-BUF 槽位放入 READY 队列，由一个模块内部媒体线程执行 RGA、MPP 和
`EncodedVideoSink::publish()`。2026-10-27 的同机实验表明，两阶段流水平均核心吞吐为
75.614 FPS，同步对照为 44.924 FPS；流水线具有已验证的稳态吞吐收益。

当前阶段优先建立“一个线程对应一个明确运行模块”的基线，并准备后续用消息邮箱表达控制面。
继续把媒体线程隐藏在 Godot 模块内部，会让线程所有权、MPP 控制和停止路径与应用层看到的
模块边界不一致。

## 决策

移除 `OpenVTDmaBufPipeline` 内部媒体线程、READY 队列和三槽背压。应用把职责拆成
`GodotRenderPipeline` 和 `MediaEncodeSession`，当前由 Godot 所有者线程同步串联：

```text
Godot / Ayagami 渲染
  → 获取 Viewport 原生纹理
  → glBlitFramebuffer
  → glFinish
  → RgbaFrameSink::submit
  → 宿主 RgaNv12FrameTransformer / 所有者线程直接同步调用 RGA
  → Nv12FrameSink::submit
  → MediaEncodeSession
  → MPP H.264
  → EncodedVideoSink::publish
```

OpenVT 只保留一个 RGBA FrameSlot。ABI v6 将获取纹理、GPU blit、GPU wait 和 RGBA
发布拆成四个语义操作；最后一个同步回调返回时，宿主 RGA 调用与编码模块已经完成处理并把
编码包复制到 `OutputManager` 队列。
RTSP/FFmpeg 输出线程继续独立，因此网络写入不会进入 Godot 所有者线程。

## 为什么现在这样做

- 线程归属明确，Godot 输出侧只有一个所有者线程；渲染、RGA、编码各自具有独立模块边界；
- RGA、MPP 和输出回调失败可以沿当前帧调用栈同步返回；
- 去除 READY/FREE 信号量、队列、三槽状态和媒体线程 join，先形成可解释的同步基线；
- 后续若引入 Actor/Active Object 控制面，可以从现有模块明确哪些命令属于 Godot、MPP
  和协议输出，再依据实测决定是否重新拆出独立编码 Actor。

## 代价和边界

- 放弃 Godot 渲染下一帧与上一帧 RGA/MPP 的跨帧重叠；
- RGA 排队/执行、MPP 取包或编码包发布回调的耗时都会直接增加 Godot 帧时间；
- 该修改优化的是所有权和控制结构，不宣称提高吞吐、降低延迟或减少内存带宽；
- 历史流水线吞吐结果继续有效，当前实现必须作为新的同步基线重新实测；
- `RgbaFrameSink` 与 `Nv12FrameSink` 都借用 DMA-BUF 到回调返回，`EncodedVideoSink` 借用编码字节到回调返回；
  编码包进入 `MediaOutputSession` 时仍复制一次。

## 重新拆线程的条件

只有测量表明同步链路无法满足目标帧周期或尾延迟要求时，才重新引入独立编码线程。至少记录：

1. Godot 渲染、GPU copy、RGA、MPP、publish 的平均值、P95、P99 和最大值；
2. 30 FPS 下 33.3 ms deadline miss 数量与连续丢帧；
3. 输出队列深度、丢包、重连和端到端延迟；
4. GPU、RGA、MPP 并行时的吞吐收益与内存带宽代价；
5. 正常退出、编码失败和网络断开时的资源释放结果。

若重新拆分，独立编码线程应成为应用可见的 `MediaEncodeStage`，通过有界帧通道和控制邮箱
通信，不再作为 `OpenVTDmaBufPipeline` 的隐藏工作线程。
