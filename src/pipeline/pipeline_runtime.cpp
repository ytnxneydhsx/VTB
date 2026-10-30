#include "vtb/pipeline/pipeline_runtime.hpp"

#include <atomic>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

#include "vtb/video/latest_frame_slot.hpp"

namespace vtb {

class PipelineRuntimeState final {
public:
    PipelineRuntimeState(
        std::unique_ptr<VideoSource> video_source,
        std::unique_ptr<FacePipeline> face_pipeline,
        PipelineRuntime::ResultHandler result_handler)
        : video_source_(std::move(video_source)),
          face_pipeline_(std::move(face_pipeline)),
          result_handler_(std::move(result_handler)) {
        if (!video_source_ || !face_pipeline_ || !result_handler_) {
            throw std::invalid_argument(
                "PipelineRuntime dependencies must not be null");
        }
    }

    ~PipelineRuntimeState() {
        close_source();
    }

    PipelineRuntimeState(const PipelineRuntimeState&) = delete;
    PipelineRuntimeState& operator=(const PipelineRuntimeState&) = delete;
    PipelineRuntimeState(PipelineRuntimeState&&) = delete;
    PipelineRuntimeState& operator=(PipelineRuntimeState&&) = delete;

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

    void capture_loop() noexcept {
        set_capture_thread_id(std::this_thread::get_id());
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

    void inference_loop() noexcept {
        set_inference_thread_id(std::this_thread::get_id());
        try {
            while (true) {
                std::optional<DecodedFrameLease> frame =
                    latest_frame_.wait_and_take();
                if (!frame) {
                    break;
                }
                std::optional<FaceTrackingResult> result =
                    face_pipeline_->process(frame->view());
                processed_frame_count_.fetch_add(1, std::memory_order_relaxed);
                result_handler_(result);
            }
        } catch (...) {
            record_failure(std::current_exception());
            request_stop();
        }
    }

    bool is_worker_thread(std::thread::id thread_id) const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return thread_id == capture_thread_id_ ||
               thread_id == inference_thread_id_;
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
        return processed_frame_count_.load(std::memory_order_relaxed);
    }

private:
    void set_capture_thread_id(std::thread::id thread_id) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        capture_thread_id_ = thread_id;
    }

    void set_inference_thread_id(std::thread::id thread_id) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        inference_thread_id_ = thread_id;
    }

    void record_failure(std::exception_ptr failure) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!failure_) {
            failure_ = std::move(failure);
        }
    }

    std::unique_ptr<VideoSource> video_source_;
    std::unique_ptr<FacePipeline> face_pipeline_;
    PipelineRuntime::ResultHandler result_handler_;
    LatestFrameSlot latest_frame_;
    mutable std::mutex mutex_;
    std::exception_ptr failure_;
    std::thread::id capture_thread_id_;
    std::thread::id inference_thread_id_;
    bool stop_requested_ = false;
    std::atomic<std::uint64_t> processed_frame_count_{0};
};

PipelineRuntime::PipelineRuntime(
    std::unique_ptr<VideoSource> video_source,
    std::unique_ptr<FacePipeline> face_pipeline,
    ResultHandler result_handler)
    : state_(std::make_shared<PipelineRuntimeState>(
          std::move(video_source),
          std::move(face_pipeline),
          std::move(result_handler))) {}

PipelineRuntime::~PipelineRuntime() {
    request_stop();
    if (called_from_worker()) {
        detach_current_and_join_other();
        return;
    }
    try {
        join_threads();
    } catch (...) {
        std::terminate();
    }
}

void PipelineRuntime::start() {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (started_ || stop_requested_ || stopped_) {
            throw std::logic_error("PipelineRuntime can only be started once");
        }
        started_ = true;
    }

    try {
        std::shared_ptr<PipelineRuntimeState> state = state_;
        inference_thread_ = std::thread(
            [state]() { state->inference_loop(); });
        capture_thread_ = std::thread(
            [state]() { state->capture_loop(); });
    } catch (...) {
        request_stop();
        join_threads();
        throw;
    }
}

void PipelineRuntime::request_stop() noexcept {
    bool signal_stop = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!stop_requested_) {
            stop_requested_ = true;
            signal_stop = true;
        }
    }
    if (signal_stop) {
        state_->request_stop();
    }
}

void PipelineRuntime::wait() {
    if (called_from_worker()) {
        throw std::logic_error("PipelineRuntime worker cannot wait for itself");
    }
    join_threads();
}

void PipelineRuntime::stop() {
    request_stop();
    if (called_from_worker()) {
        return;
    }
    join_threads();
}

void PipelineRuntime::rethrow_if_failed() const {
    state_->rethrow_if_failed();
}

std::uint64_t PipelineRuntime::published_frame_count() const noexcept {
    return state_->published_frame_count();
}

std::uint64_t PipelineRuntime::replaced_frame_count() const noexcept {
    return state_->replaced_frame_count();
}

std::uint64_t PipelineRuntime::processed_frame_count() const noexcept {
    return state_->processed_frame_count();
}

bool PipelineRuntime::called_from_worker() const noexcept {
    return state_->is_worker_thread(std::this_thread::get_id());
}

void PipelineRuntime::join_threads() {
    std::lock_guard<std::mutex> join_lock(join_mutex_);
    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }
    if (inference_thread_.joinable()) {
        inference_thread_.join();
    }
    state_->close_source();
    std::lock_guard<std::mutex> state_lock(state_mutex_);
    stopped_ = true;
}

void PipelineRuntime::detach_current_and_join_other() noexcept {
    try {
        std::lock_guard<std::mutex> join_lock(join_mutex_);
        const std::thread::id current_thread = std::this_thread::get_id();
        if (capture_thread_.joinable()) {
            if (capture_thread_.get_id() == current_thread) {
                capture_thread_.detach();
            } else {
                capture_thread_.join();
            }
        }
        if (inference_thread_.joinable()) {
            if (inference_thread_.get_id() == current_thread) {
                inference_thread_.detach();
            } else {
                inference_thread_.join();
            }
        }
        state_->close_source();
        state_.reset();
    } catch (...) {
        std::terminate();
    }
}

}  // namespace vtb
