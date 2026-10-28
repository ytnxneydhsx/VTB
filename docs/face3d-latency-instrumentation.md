# Face3D 分阶段与端到端延迟设计

## 为什么需要这条链路

当前 Face3D 推流已经分别记录 Python 推理、RGA、MPP 和发布的平均耗时，但 Python 向 C++ 发送的是无帧头 BGR 字节流，帧号和前级时间戳在进程边界丢失。Mac 浏览器也无法确认当前显示画面对应 RK 的哪一帧。因此现有数据只能描述模块吞吐，不能证明同一帧从采集完成到浏览器显示经历的时间。

首版目标是让 RK 内每个阶段和 Mac 浏览器显示使用同一个可换算的时间域，并在浏览器显示最近窗口的当前值、P50、P95 和 P99。系统墙钟不参与延迟相减，避免 NTP 校时或人工调整造成负数和尖峰。

## 时钟模型

RK 上的 Python 和 C++ 显式使用 `CLOCK_MONOTONIC` 纳秒时间。这个时钟在一次启动期间由所有进程共享。

Mac 浏览器使用 `performance.now()`。诊断页定期向 RK 时钟接口发送一组四时间戳请求：浏览器记录发送和接收时间，RK 返回请求到达和响应发出时的单调时间。每组选择 RTT 最低的样本，得到 `RK monotonic -> browser monotonic` 的偏移和本次估计的不确定度。长时间运行周期性重采样以限制晶振漂移。

系统 NTP/chrony 只用于让日志墙钟一致，不作为端到端延迟正确性的前提。

## 帧关联

Python 到 C++ 的输入改为固定帧头加 BGR24 负载。帧头包含协议版本、会话 ID、帧 ID、采集序号、Python 各阶段的 RK 单调时间以及负载长度。C++ 校验帧头后继续补充接收、RGA、MPP、输出入队和 RTSP 写出时间。

诊断模式在画面底部绘制三份相同的高对比度二进制标记，编码同步字节、32 位帧 ID 和校验字节。浏览器在实际视频帧回调中读取标记，以帧 ID 关联 RK 的阶段记录。标记只服务诊断，正式画面可以关闭。

## 阶段定义

```text
capture_ready
  -> inference_start
  -> detector_start / detector_end
  -> landmark_start / landmark_end
  -> points_ready
  -> canvas_ready
  -> cpp_frame_received
  -> rga_done
  -> mpp_done
  -> output_published
  -> rtsp_write_start / rtsp_write_end
  -> browser_presented
```

`capture_ready` 是 OpenCV `capture.read()` 返回完整帧的时刻，不包含摄像头曝光、USB 传输和 MJPEG 解码的前半段。需要曝光到屏幕的物理延迟时，再接入 V4L2 `VIDIOC_DQBUF` 时间戳或光学测量。

## 更简单方案与取舍

只打印各模块平均耗时改动最小，但无法关联同一帧，也无法区分网络缓冲和浏览器显示缓冲。只同步系统墙钟仍然不能直接比较两台机器各自的单调时钟，而且墙钟可能跳变。

固定帧头、浏览器时钟映射和画面帧标记增加少量诊断开销，换来可验证的逐帧端到端数据。首版使用 HTTP 时钟请求和 trace 轮询，保持浏览器零安装；局域网最低 RTT 和估计误差会与延迟一起显示。若 HTTP 调度抖动超过目标，再替换为 WebSocket，不改变帧协议和时间模型。

## 验收指标

- RK 与浏览器时钟校准能够报告 offset、最低 RTT 和误差上界。
- 浏览器连续识别帧 ID，并能找到同一帧的 RK trace。
- 页面显示各阶段 current、P50、P95、P99。
- 页面显示采集完成到浏览器呈现的端到端延迟及同步误差。
- 运行至少 300 帧，trace 无协议错误，视频无新增输出队列丢帧。
