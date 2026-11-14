#pragma once

#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>

#include "vtb/messaging/bounded_fifo_channel.hpp"
#include "vtb/messaging/video/decoded_frame_lease.hpp"
#include "vtb/video/video_source.hpp"

namespace vtb {

using VideoFrameChannel =
    messaging::BoundedFifoChannel<DecodedFrameLease>;

/** @brief 从 VideoSource 读取帧并发布到帧通道的独立采集阶段。 */
class VideoCaptureStage final {
public:
    VideoCaptureStage(
        std::unique_ptr<VideoSource> video_source,
        VideoFrameChannel& output_channel);
    ~VideoCaptureStage();

    VideoCaptureStage(const VideoCaptureStage&) = delete;
    VideoCaptureStage& operator=(const VideoCaptureStage&) = delete;
    VideoCaptureStage(VideoCaptureStage&&) = delete;
    VideoCaptureStage& operator=(VideoCaptureStage&&) = delete;

    void start();
    void request_stop() noexcept;
    void stop();
    void rethrow_if_failed() const;

    std::uint64_t published_frame_count() const noexcept;
    std::uint64_t dropped_frame_count() const noexcept;

private:
    void thread_main() noexcept;
    void record_failure(std::exception_ptr failure) noexcept;
    void shutdown_noexcept() noexcept;

    std::unique_ptr<VideoSource> video_source_;
    VideoFrameChannel& output_channel_;
    mutable std::mutex failure_mutex_;
    std::exception_ptr failure_;
    std::thread worker_;
    std::atomic<bool> stop_requested_{false};
    bool started_ = false;
    bool source_closed_ = false;
};

}  // namespace vtb
