#include "pipeline/video_capture_stage.hpp"

#include <optional>
#include <stdexcept>
#include <utility>

namespace vtb {

VideoCaptureStage::VideoCaptureStage(
    std::unique_ptr<VideoSource> video_source,
    VideoFrameChannel& output_channel)
    : video_source_(std::move(video_source)),
      output_channel_(output_channel) {
    if (!video_source_) {
        throw std::invalid_argument(
            "VideoCaptureStage video source must not be null");
    }
}

VideoCaptureStage::~VideoCaptureStage() {
    shutdown_noexcept();
}

void VideoCaptureStage::start() {
    if (started_ || stop_requested_.load()) {
        throw std::logic_error("VideoCaptureStage can only be started once");
    }
    worker_ = std::thread(&VideoCaptureStage::thread_main, this);
    started_ = true;
}

void VideoCaptureStage::request_stop() noexcept {
    const bool already_requested =
        stop_requested_.exchange(true);
    if (already_requested) {
        return;
    }
    video_source_->request_stop();
}

void VideoCaptureStage::stop() {
    request_stop();
    if (worker_.joinable()) {
        worker_.join();
    }
    output_channel_.close();
    if (!source_closed_) {
        video_source_->close();
        source_closed_ = true;
    }
    started_ = false;
}

void VideoCaptureStage::rethrow_if_failed() const {
    std::exception_ptr failure;
    {
        std::lock_guard<std::mutex> lock(failure_mutex_);
        failure = failure_;
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
}

std::uint64_t VideoCaptureStage::published_frame_count() const noexcept {
    return output_channel_.published_count();
}

std::uint64_t VideoCaptureStage::dropped_frame_count() const noexcept {
    return output_channel_.dropped_count();
}

void VideoCaptureStage::thread_main() noexcept {
    bool reached_end_of_stream = false;
    try {
        while (!stop_requested_.load()) {
            std::optional<DecodedFrameLease> frame = video_source_->read();
            if (!frame.has_value()) {
                if (!stop_requested_.load()) {
                    reached_end_of_stream = true;
                }
                break;
            }

            const messaging::ChannelPublishResult result =
                output_channel_.publish(std::move(frame.value()));
            if (result == messaging::ChannelPublishResult::closed ||
                result == messaging::ChannelPublishResult::finished) {
                break;
            }
        }
    } catch (...) {
        record_failure(std::current_exception());
        output_channel_.close();
        return;
    }

    if (reached_end_of_stream) {
        output_channel_.finish();
    } else {
        output_channel_.close();
    }
}

void VideoCaptureStage::record_failure(
    std::exception_ptr failure) noexcept {
    std::lock_guard<std::mutex> lock(failure_mutex_);
    if (!failure_) {
        failure_ = std::move(failure);
    }
}

void VideoCaptureStage::shutdown_noexcept() noexcept {
    try {
        stop();
    } catch (...) {
    }
}

}  // namespace vtb
