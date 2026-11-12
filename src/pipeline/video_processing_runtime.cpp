#include "vtb/pipeline/video_processing_runtime.hpp"

#include <atomic>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

#include "vtb/video/latest_frame_slot.hpp"

namespace vtb {

class VideoProcessingRuntimeState final {
public:
    VideoProcessingRuntimeState(
        std::unique_ptr<VideoSource> video_source,
        std::unique_ptr<VideoFrameConsumer> frame_consumer)
        : video_source_(std::move(video_source)),
          frame_consumer_(std::move(frame_consumer)) {
        if (!video_source_ || !frame_consumer_) {
            throw std::invalid_argument(
                "VideoProcessingRuntime dependencies must not be null");
        }
    }

    ~VideoProcessingRuntimeState() = default;

    VideoProcessingRuntimeState(const VideoProcessingRuntimeState&) = delete;
    VideoProcessingRuntimeState& operator=(
        const VideoProcessingRuntimeState&) = delete;
    VideoProcessingRuntimeState(VideoProcessingRuntimeState&&) = delete;
    VideoProcessingRuntimeState& operator=(
        VideoProcessingRuntimeState&&) = delete;

    void request_stop() noexcept {
        bool signal_stop = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!stop_requested_) {
                stop_requested_ = true;
                signal_stop = true;
            }
        }
        if (signal_stop) {
            video_source_->request_stop();
            latest_frame_.close();
        }
    }

    void close_source() noexcept {
        video_source_->close();
    }

    void begin_source() {
        VideoStreamControlReply reply = control_protocol_.exchange(
            VideoStreamControlAction::begin_source, generation_);
        reply.rethrow_if_failed();
        source_active_ = true;
    }

    void retire_source() noexcept {
        if (!source_active_) {
            return;
        }
        try {
            VideoStreamControlReply reply = control_protocol_.exchange(
                VideoStreamControlAction::retire_source, generation_);
            reply.rethrow_if_failed();
        } catch (...) {
            record_failure(std::current_exception());
        }
        source_active_ = false;
    }

    void shutdown_consumer() noexcept {
        if (consumer_shutdown_) {
            return;
        }
        try {
            VideoStreamControlReply reply = control_protocol_.exchange(
                VideoStreamControlAction::shutdown_consumer, generation_);
            reply.rethrow_if_failed();
        } catch (...) {
            record_failure(std::current_exception());
        }
        consumer_shutdown_ = true;
        control_protocol_.close();
    }

    void capture_loop() noexcept {
        bool completed = false;
        try {
            while (true) {
                std::optional<DecodedFrameLease> frame = video_source_->read();
                if (!frame) {
                    break;
                }
                if (!latest_frame_.publish(std::move(*frame))) {
                    break;
                }
            }
            completed = true;
        } catch (...) {
            record_failure(std::current_exception());
        }
        if (completed) {
            latest_frame_.finish();
        } else {
            latest_frame_.close();
        }
    }

    void consumer_loop() noexcept {
        std::optional<VideoStreamControlRequest> begin_request =
            control_protocol_.wait_request();
        if (!begin_request) {
            return;
        }
        if (begin_request->action() !=
                VideoStreamControlAction::begin_source ||
            begin_request->generation() != generation_) {
            const std::exception_ptr failure = std::make_exception_ptr(
                std::logic_error(
                    "Consumer expected begin_source for the active generation"));
            control_protocol_.fail(*begin_request, failure);
            record_failure(failure);
            control_protocol_.close();
            return;
        }
        try {
            frame_consumer_->begin_source(generation_);
            control_protocol_.complete(*begin_request);
        } catch (...) {
            const std::exception_ptr failure = std::current_exception();
            control_protocol_.fail(*begin_request, failure);
            record_failure(failure);
            request_stop();
            control_protocol_.close();
            return;
        }

        try {
            while (true) {
                std::optional<DecodedFrameLease> frame =
                    latest_frame_.wait_and_take();
                if (!frame) {
                    break;
                }
                frame_consumer_->consume(std::move(*frame));
                processed_frame_count_.fetch_add(1);
            }
        } catch (...) {
            record_failure(std::current_exception());
            request_stop();
        }

        std::optional<VideoStreamControlRequest> retire_request =
            control_protocol_.wait_request();
        if (!retire_request) {
            frame_consumer_->retire_source(generation_);
            return;
        }
        if (retire_request->action() !=
                VideoStreamControlAction::retire_source ||
            retire_request->generation() != generation_) {
            const std::exception_ptr failure = std::make_exception_ptr(
                std::logic_error(
                    "Consumer expected retire_source for the active generation"));
            control_protocol_.fail(*retire_request, failure);
            record_failure(failure);
            frame_consumer_->retire_source(generation_);
            control_protocol_.close();
            return;
        }
        frame_consumer_->retire_source(generation_);
        control_protocol_.complete(*retire_request);

        std::optional<VideoStreamControlRequest> shutdown_request =
            control_protocol_.wait_request();
        if (!shutdown_request) {
            return;
        }
        if (shutdown_request->action() !=
                VideoStreamControlAction::shutdown_consumer ||
            shutdown_request->generation() != generation_) {
            const std::exception_ptr failure = std::make_exception_ptr(
                std::logic_error(
                    "Consumer expected shutdown_consumer after source retirement"));
            control_protocol_.fail(*shutdown_request, failure);
            record_failure(failure);
            control_protocol_.close();
            return;
        }
        control_protocol_.complete(*shutdown_request);
        control_protocol_.close();
    }

    void rethrow_if_failed() const {
        std::exception_ptr failure;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            failure = failure_;
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    std::uint64_t published_frame_count() const noexcept {
        return latest_frame_.published_count();
    }

    std::uint64_t replaced_frame_count() const noexcept {
        return latest_frame_.replaced_count();
    }

    std::uint64_t processed_frame_count() const noexcept {
        return processed_frame_count_.load();
    }

private:
    void record_failure(std::exception_ptr failure) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!failure_) {
            failure_ = std::move(failure);
        }
    }

    std::unique_ptr<VideoSource> video_source_;
    std::unique_ptr<VideoFrameConsumer> frame_consumer_;
    LatestFrameSlot latest_frame_;
    VideoStreamControlProtocol control_protocol_;
    mutable std::mutex mutex_;
    std::exception_ptr failure_;
    bool stop_requested_ = false;
    bool source_active_ = false;
    bool consumer_shutdown_ = false;
    VideoStreamGeneration generation_{1};
    std::atomic<std::uint64_t> processed_frame_count_{0};
};

VideoProcessingRuntime::VideoProcessingRuntime(
    std::unique_ptr<VideoSource> video_source,
    std::unique_ptr<VideoFrameConsumer> frame_consumer)
    : state_(std::make_unique<VideoProcessingRuntimeState>(
          std::move(video_source),
          std::move(frame_consumer))) {}

VideoProcessingRuntime::~VideoProcessingRuntime() {
    request_stop();
    join_threads();
}

void VideoProcessingRuntime::start() {
    if (started_ || stop_requested_) {
        throw std::logic_error(
            "VideoProcessingRuntime can only be started once");
    }
    started_ = true;

    try {
        consumer_thread_ = std::thread(
            &VideoProcessingRuntimeState::consumer_loop,
            state_.get());
        state_->begin_source();
        capture_thread_ = std::thread(
            &VideoProcessingRuntimeState::capture_loop,
            state_.get());
    } catch (...) {
        request_stop();
        join_threads();
        throw;
    }
}

void VideoProcessingRuntime::request_stop() noexcept {
    if (stop_requested_) {
        return;
    }
    stop_requested_ = true;
    state_->request_stop();
}

void VideoProcessingRuntime::wait() {
    join_threads();
}

void VideoProcessingRuntime::stop() {
    request_stop();
    join_threads();
}

void VideoProcessingRuntime::rethrow_if_failed() const {
    state_->rethrow_if_failed();
}

std::uint64_t VideoProcessingRuntime::published_frame_count() const noexcept {
    return state_->published_frame_count();
}

std::uint64_t VideoProcessingRuntime::replaced_frame_count() const noexcept {
    return state_->replaced_frame_count();
}

std::uint64_t VideoProcessingRuntime::processed_frame_count() const noexcept {
    return state_->processed_frame_count();
}

void VideoProcessingRuntime::join_threads() {
    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }
    if (consumer_thread_.joinable()) {
        state_->retire_source();
        state_->shutdown_consumer();
        consumer_thread_.join();
    }
    state_->close_source();
}

}  // namespace vtb
