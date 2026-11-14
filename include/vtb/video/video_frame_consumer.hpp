#pragma once

#include "vtb/messaging/video/decoded_frame_lease.hpp"
#include "vtb/video/video_stream_control_protocol.hpp"

namespace vtb {

/**
 * @brief 统一的视频帧消费接口。
 *
 * begin_source()、consume() 和 retire_source() 全部由同一个消费线程顺序调用。
 * begin_source() 在采集线程启动前完成；retire_source() 在该代最后一帧处理结束后
 * 释放源相关缓存。
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

    /** @brief 在第一帧到达前准备一代视频源。 */
    virtual void begin_source(VideoStreamGeneration generation) = 0;

    /**
     * @brief 同步处理一帧视频。
     * @param frame 当前消费者独占的有效帧租约。
     *
     * 实现必须在函数返回前完成处理并释放租约，禁止保存租约或帧内存指针供返回后使用。
     * 当前运行时等待同步 consume() 完成，再处理该代 retire_source 控制请求。
     */
    virtual void consume(DecodedFrameLease frame) = 0;

    /**
     * @brief 在该代所有帧处理完成后释放源相关缓存。
     *
     * 调用发生在消费线程，并且同步 RGA/RKNN 处理已经返回。实现不得抛出异常。
     */
    virtual void retire_source(VideoStreamGeneration generation) noexcept = 0;
};

}  // namespace vtb
