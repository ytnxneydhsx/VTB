#include "pipeline/godot_render_stage.hpp"

#include <chrono>
#include <optional>
#include <stdexcept>
#include <utility>

#if defined(__linux__)
#include <pthread.h>
#endif

namespace vtb {

GodotRenderStage::GodotRenderStage(
    std::unique_ptr<GodotRenderPipeline> render_pipeline,
    Live2DTrackingChannel& input_channel,
    const std::atomic<bool>& process_stop_requested)
    : render_pipeline_(std::move(render_pipeline)),
      input_channel_(input_channel),
      process_stop_requested_(process_stop_requested) {
    if (!render_pipeline_) {
        throw std::invalid_argument(
            "GodotRenderStage render pipeline must not be null");
    }
}

GodotRenderStage::~GodotRenderStage() {
    shutdown_noexcept();
}

void GodotRenderStage::start() {
    if (started_) {
        throw std::logic_error("GodotRenderStage can only be started once");
    }
    started_ = true;
    worker_ = std::thread(&GodotRenderStage::thread_main, this);

    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this]() { return initialization_finished_; });
    const std::exception_ptr initialization_error = failure_;
    lock.unlock();
    if (initialization_error) {
        worker_.join();
        std::rethrow_exception(initialization_error);
    }
}

void GodotRenderStage::begin_frames() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        frames_permitted_ = true;
    }
    condition_.notify_all();
}

void GodotRenderStage::wait_for_frame_loop() {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this]() { return frame_loop_finished_; });
    const std::exception_ptr frame_loop_failure = failure_;
    lock.unlock();
    if (frame_loop_failure) {
        std::rethrow_exception(frame_loop_failure);
    }
}

void GodotRenderStage::finish() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cleanup_permitted_ = true;
    }
    condition_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    rethrow_if_failed();
}

void GodotRenderStage::thread_main() noexcept {
#if defined(__linux__)
    (void)pthread_setname_np(pthread_self(), "vtb-render");
#endif
    std::unique_ptr<GodotRenderPipeline> render_pipeline =
        std::move(render_pipeline_);
    try {
        render_pipeline->initialize();
        mark_initialization_finished();

        bool run_frames = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this]() {
                return frames_permitted_ || cleanup_permitted_;
            });
            run_frames = frames_permitted_ && !cleanup_permitted_;
        }

        if (run_frames) {
            try {
                run_frame_loop(*render_pipeline);
            } catch (...) {
                remember_failure(std::current_exception());
            }
        }
        mark_frame_loop_finished();

        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this]() { return cleanup_permitted_; });
        }
        finish_pipeline(*render_pipeline);
    } catch (...) {
        remember_failure(std::current_exception());
        mark_initialization_finished();
        mark_frame_loop_finished();
    }
}

void GodotRenderStage::run_frame_loop(
    GodotRenderPipeline& render_pipeline) {
    auto next_frame_deadline = std::chrono::steady_clock::now();
    while (!process_stop_requested_.load() &&
           !internal_stop_requested_.load()) {
        std::optional<Live2DTrackingSnapshot> snapshot =
            input_channel_.try_pop();
        if (snapshot.has_value()) {
            render_pipeline.submit(snapshot.value());
        } else if (input_channel_.finished()) {
            break;
        }

        const auto result = render_pipeline.run_frame();
        if (result != GodotRenderPipeline::FrameResult::skipped) {
            ++iterations_;
        }
        if (result == GodotRenderPipeline::FrameResult::finished) {
            break;
        }
        if (render_pipeline.iteration_limit() > 0 &&
            iterations_ >= render_pipeline.iteration_limit()) {
            break;
        }

        next_frame_deadline += render_pipeline.frame_period();
        const auto current_time = std::chrono::steady_clock::now();
        if (next_frame_deadline < current_time) {
            next_frame_deadline = current_time;
        }
        std::this_thread::sleep_until(next_frame_deadline);
    }
}

void GodotRenderStage::finish_pipeline(
    GodotRenderPipeline& render_pipeline) noexcept {
    try {
        render_pipeline.finish();
    } catch (...) {
        remember_failure(std::current_exception());
    }
}

void GodotRenderStage::mark_initialization_finished() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        initialization_finished_ = true;
    }
    condition_.notify_all();
}

void GodotRenderStage::mark_frame_loop_finished() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        frame_loop_finished_ = true;
    }
    condition_.notify_all();
}

void GodotRenderStage::remember_failure(
    std::exception_ptr failure) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!failure_) {
        failure_ = std::move(failure);
    }
}

void GodotRenderStage::rethrow_if_failed() const {
    std::exception_ptr failure;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        failure = failure_;
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
}

void GodotRenderStage::shutdown_noexcept() noexcept {
    internal_stop_requested_.store(true);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        frames_permitted_ = true;
        cleanup_permitted_ = true;
    }
    condition_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

}  // namespace vtb
