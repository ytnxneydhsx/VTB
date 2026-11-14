#pragma once

#include <optional>

#include "vtb/messaging/video/decoded_frame_lease.hpp"

namespace vtb {

class VideoSource {
public:
    VideoSource() = default;
    virtual ~VideoSource() = default;

    VideoSource(const VideoSource&) = delete;
    VideoSource& operator=(const VideoSource&) = delete;
    VideoSource(VideoSource&&) = delete;
    VideoSource& operator=(VideoSource&&) = delete;

    /**
     * @brief 读取并交付下一帧视频的租约。
     *
     * 返回的 DecodedFrameLease 保证其 FrameView 引用的像素存储在租约
     * 有效期间保持可用。租约析构时会释放 CPU 存储，或者把 V4L2
     * 缓冲区归还给驱动。
     *
     * @return 成功取得帧时返回包含 DecodedFrameLease 的 optional；视频源
     *         已停止、已结束或无法继续交付帧时返回 std::nullopt。
     * @note 该调用可能等待下一帧。设备、格式或帧数据错误可以通过异常报告。
     */
    virtual std::optional<DecodedFrameLease> read() = 0;

    /**
     * @brief 请求视频源协作式停止读取。
     *
     * 该函数用于通知正在运行的读取循环退出，使当前或后续 read() 最终
     * 不再交付新帧。它只发出停止请求，不负责等待读取线程结束，也不保证
     * 底层设备和缓冲区已经释放。
     */
    virtual void request_stop() noexcept = 0;

    /**
     * @brief 关闭视频源并释放底层采集资源。
     *
     * 实现会先请求停止，再释放摄像头句柄、文件描述符或缓冲池等资源。
     * 调用方应先确保使用该视频源的读取线程已经退出。
     */
    virtual void close() noexcept = 0;
};

}  // namespace vtb
