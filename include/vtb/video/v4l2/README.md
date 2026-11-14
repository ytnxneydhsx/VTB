# V4L2 视频缓冲资源边界

这个目录保存 V4L2 摄像头采集链路中的独立资源类。声明位于
`include/vtb/video/v4l2`，实现位于对应的 `src/video/v4l2`。

## 为什么拆开

原来的 `camera_source.cpp` 同时定义 `CameraSource`、`V4l2BufferPool` 和
`BufferMapping`。其中 `CameraSource` 只承担 `VideoSource` 适配职责；真正拥有
摄像头对象、MMAP、DMA-BUF fd、DQBUF/QBUF 状态的是后两个资源类。把资源类隐藏在
`CameraSource` 的实现文件中，会让 `shared_ptr<V4l2BufferPool>` 的实际生命周期、
五法则选择和租约归还路径难以从目录及头文件中直接看出。

拆分后的职责是：

- `CameraSource`：创建缓冲池，并转发 `read/request_stop/close`。
- `V4l2BufferPool`：以值成员独占 `V4l2Camera`，并拥有缓冲区集合和
  DQBUF/QBUF 状态。
- `V4l2Camera`：在 Pool 内原地配置设备，独占唯一摄像头 fd。
- `V4l2BufferMapping`：独占一个槽位的 MMAP 映射和可选 DMA-BUF fd。
- `DecodedFrameLease`：跨线程移动一帧的访问权，并通过归还动作延长缓冲池生命期。

```text
CameraSource
    └── shared_ptr<V4l2BufferPool>
            ├── V4l2Camera
            │       └── 唯一设备 fd
            ├── vector<V4l2BufferMapping>
            └── DecodedFrameLease.return_action
                    └── shared_ptr<V4l2BufferPool>
```

## 所有权规则

`CameraSource::close()` 只释放自己持有的缓冲池句柄。已经发布到消息通道的
`DecodedFrameLease` 继续持有同一个缓冲池，保证其 `FrameView` 借用的 MMAP 地址和
DMA-BUF fd 有效。最后一份租约释放后，缓冲池才执行 STREAMOFF，清空映射，
随后由 `V4l2Camera` 值成员析构并关闭设备 fd。

该结构使 Pool 的共享生命期直接覆盖 Camera 和 Mapping，避免为设备 fd 额外执行
`F_DUPFD_CLOEXEC`。容量一消息通道和单帧租约协议保持现有行为。设备行为和性能仍需
通过后续统一编译与实机验证确认。
