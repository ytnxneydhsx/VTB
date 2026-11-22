#pragma once

#include <atomic>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>

#include "pipeline/face_tracking_stage.hpp"
#include "pipeline/godot_render_pipeline.hpp"

namespace vtb {

/**
 * @brief 在专用所有者线程中消费 Live2D 快照并驱动同步 Godot 渲染主体。
 *
 * 本类负责线程、输入通道、帧调度、停止和异常传播。工作线程只独占
 * GodotRenderPipeline；Pipeline 内部管理 LibGodotInstance、直接 DMA-BUF 渲染和 RGBA 同步借用。
 * input_channel_ 和 process_stop_requested_ 是对 VtbApplication::run() 局部对象的借用；
 * 调用方在本 Stage 完成 finish() 和析构前保持二者有效。
 */
class GodotRenderStage final {
public:
    GodotRenderStage(
        std::unique_ptr<GodotRenderPipeline> render_pipeline,
        Live2DTrackingChannel& input_channel,
        const std::atomic<bool>& process_stop_requested);
    ~GodotRenderStage();

    GodotRenderStage(const GodotRenderStage&) = delete;
    GodotRenderStage& operator=(const GodotRenderStage&) = delete;
    GodotRenderStage(GodotRenderStage&&) = delete;
    GodotRenderStage& operator=(GodotRenderStage&&) = delete;

    void start();
    void begin_frames() noexcept;
    void wait_for_frame_loop();
    void finish();

private:
    void thread_main() noexcept;
    void run_frame_loop(GodotRenderPipeline& render_pipeline);
    void finish_pipeline(GodotRenderPipeline& render_pipeline) noexcept;
    void mark_initialization_finished() noexcept;
    void mark_frame_loop_finished() noexcept;
    void remember_failure(std::exception_ptr failure) noexcept;
    void rethrow_if_failed() const;
    void shutdown_noexcept() noexcept;

    std::unique_ptr<GodotRenderPipeline> render_pipeline_;
    Live2DTrackingChannel& input_channel_;
    const std::atomic<bool>& process_stop_requested_;
    std::atomic<bool> internal_stop_requested_{false};

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::exception_ptr failure_;
    std::thread worker_;
    unsigned long iterations_ = 0;
    bool initialization_finished_ = false;
    bool frames_permitted_ = false;
    bool frame_loop_finished_ = false;
    bool cleanup_permitted_ = false;
    bool started_ = false;
};

}  // namespace vtb
