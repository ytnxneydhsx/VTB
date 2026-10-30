#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "vtb/face/face_pipeline.hpp"
#include "vtb/video/video_source.hpp"

namespace vtb {

class PipelineRuntimeState;

class PipelineRuntime {
public:
    using ResultHandler = std::function<void(const FaceTrackingResult&)>;

    PipelineRuntime(
        std::unique_ptr<VideoSource> video_source,
        std::unique_ptr<FacePipeline> face_pipeline,
        ResultHandler result_handler);
    ~PipelineRuntime();

    PipelineRuntime(const PipelineRuntime&) = delete;
    PipelineRuntime& operator=(const PipelineRuntime&) = delete;
    PipelineRuntime(PipelineRuntime&&) = delete;
    PipelineRuntime& operator=(PipelineRuntime&&) = delete;

    void start();
    void request_stop() noexcept;
    void wait();
    void stop();
    void rethrow_if_failed() const;

    std::uint64_t published_frame_count() const noexcept;
    std::uint64_t replaced_frame_count() const noexcept;
    std::uint64_t processed_frame_count() const noexcept;

private:
    bool called_from_worker() const noexcept;
    void join_threads();
    void detach_current_and_join_other() noexcept;

    std::shared_ptr<PipelineRuntimeState> state_;
    std::thread capture_thread_;
    std::thread inference_thread_;
    mutable std::mutex state_mutex_;
    std::mutex join_mutex_;
    bool started_ = false;
    bool stop_requested_ = false;
    bool stopped_ = false;
};

}  // namespace vtb
