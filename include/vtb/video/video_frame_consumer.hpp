#pragma once

#include "vtb/video/frame.hpp"

namespace vtb {

/**
 * @brief 统一的视频帧消费接口。
 *
 * 消费者按值接收 DecodedFrameLease，从而在 consume() 执行期间独占一帧的租约。
 * consume() 返回或抛出异常后，租约析构并释放底层 CPU 存储，或者把 V4L2 缓冲区
 * 归还驱动。实现类可以执行人脸推理、编码、录像或其他同步帧处理。
 */
class VideoFrameConsumer {
public:
    VideoFrameConsumer() = default;
    virtual ~VideoFrameConsumer() = default;

    VideoFrameConsumer(const VideoFrameConsumer&) = delete;
    VideoFrameConsumer& operator=(const VideoFrameConsumer&) = delete;
    VideoFrameConsumer(VideoFrameConsumer&&) = delete;
    VideoFrameConsumer& operator=(VideoFrameConsumer&&) = delete;

    /**
     * @brief 同步处理一帧视频。
     * @param frame 当前消费者独占的有效帧租约。
     *
     * 实现必须在函数返回前完成处理并释放租约，禁止保存租约或帧内存指针供返回后使用。
     * 当前运行时仅等待同步 consume() 完成，没有异步消费者的 drain/stop 协议。
     */
    virtual void consume(DecodedFrameLease frame) = 0;
};

}  // namespace vtb
