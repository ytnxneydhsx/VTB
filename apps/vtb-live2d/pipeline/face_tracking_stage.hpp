#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "live2d_tracking_snapshot.hpp"
#include "pipeline/live2d_tracking_pipeline.hpp"
#include "pipeline/video_capture_stage.hpp"
#include "vtb/messaging/bounded_fifo_channel.hpp"

namespace vtb {

using Live2DTrackingChannel =
    messaging::BoundedFifoChannel<Live2DTrackingSnapshot>;

/**
 * @brief 消费帧租约、执行人脸处理和映射，并发布 Live2D 快照的独立阶段。
 */
class FaceTrackingStage final {
public:
    FaceTrackingStage(
        std::unique_ptr<Live2DTrackingPipeline> tracking_pipeline,
        VideoFrameChannel& input_channel,
        Live2DTrackingChannel& output_channel);
    ~FaceTrackingStage();

    FaceTrackingStage(const FaceTrackingStage&) = delete;
    FaceTrackingStage& operator=(const FaceTrackingStage&) = delete;
    FaceTrackingStage(FaceTrackingStage&&) = delete;
    FaceTrackingStage& operator=(FaceTrackingStage&&) = delete;

    void start();
    void request_stop() noexcept;
    void stop();
    void rethrow_if_failed() const;

    Live2DTrackingStatus status() const;
    void request_calibration_reset() noexcept;
    bool set_view(const Live2DTrackingViewState& view) noexcept;
    std::uint64_t processed_frame_count() const noexcept;

private:
    void thread_main() noexcept;
    void process_frame(DecodedFrameLease frame);
    messaging::ChannelPublishResult publish_snapshot_locked();
    void mark_initialization_finished() noexcept;
    void record_failure(std::exception_ptr failure) noexcept;
    void shutdown_noexcept() noexcept;

    std::unique_ptr<Live2DTrackingPipeline> tracking_pipeline_;
    const std::size_t calibration_required_;
    VideoFrameChannel& input_channel_;
    Live2DTrackingChannel& output_channel_;

    mutable std::mutex lifecycle_mutex_;
    std::condition_variable lifecycle_condition_;
    std::exception_ptr failure_;
    std::thread worker_;
    bool initialization_finished_ = false;
    bool started_ = false;

    mutable std::mutex state_mutex_;
    std::optional<Live2DMappingResult> latest_mapping_;
    std::uint64_t snapshot_generation_ = 0;
    Live2DTrackingViewState view_;

    std::atomic<std::uint64_t> reset_generation_{0};
    std::atomic<bool> reset_pending_{false};
    std::uint64_t applied_reset_generation_ = 0;
    std::atomic<std::uint64_t> processed_frame_count_{0};
    VideoStreamGeneration generation_{1};
};

}  // namespace vtb
