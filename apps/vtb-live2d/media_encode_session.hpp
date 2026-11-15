#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "vtb/output/encoded_video_sink.hpp"
#include "vtb/render/nv12_frame_sink.hpp"

namespace vtb {

class MediaEncodeSessionConfig final {
public:
    std::uint32_t uvc_fps = 0;
    std::string metrics_path;
    std::chrono::milliseconds metrics_report_interval{60000};
};

/**
 * @brief 同步消费 NV12 DMA-BUF，并用独立 MPP 会话生成 H.264 和可选 MJPEG。
 *
 * 本对象不创建线程。Nv12FrameSink 回调、MPP 调用和 EncodedVideoSink 发布均在
 * GodotRenderStage 的所有者线程内完成；协议输出仍由 MediaOutputSession 的工作线程负责。
 */
class MediaEncodeSession final {
public:
    MediaEncodeSession(
        EncodedVideoSink output_sink,
        MediaEncodeSessionConfig config);
    ~MediaEncodeSession();

    MediaEncodeSession(const MediaEncodeSession&) = delete;
    MediaEncodeSession& operator=(const MediaEncodeSession&) = delete;
    MediaEncodeSession(MediaEncodeSession&&) = delete;
    MediaEncodeSession& operator=(MediaEncodeSession&&) = delete;

    Nv12FrameSink sink() noexcept;
    void rethrow_if_failed() const;
    void finish();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vtb
