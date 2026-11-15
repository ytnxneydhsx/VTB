#include "pipeline/godot_render_pipeline.hpp"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <utility>

#include "libgodot_instance.hpp"
#include "vtb/hardware/rga/rga_nv12_frame_transformer.hpp"
#include "vtb/instrumentation/metrics_jsonl_reporter.hpp"
#include "vtb/instrumentation/timer.hpp"
#include "vtb/time/monotonic_clock.hpp"

namespace vtb {

class GodotRenderPipeline::Impl final {
public:
    Impl(
        const std::vector<std::string>& passthrough_arguments,
        unsigned long iteration_limit,
        GodotRenderPipelineConfig config,
        Nv12FrameSink nv12_sink)
        : passthrough_arguments_(passthrough_arguments),
          iteration_limit_(iteration_limit),
          config_(std::move(config)),
          rga_transformer_(config_.rga_dma_heap_path),
          nv12_sink_(nv12_sink),
          prepare_timer_(timers_.register_timer("live2d.parameter_prepare")),
          iteration_timer_(timers_.register_timer("live2d.godot_iteration")),
          texture_timer_(timers_.register_timer("live2d.viewport_texture")),
          blit_timer_(timers_.register_timer("live2d.gpu_blit")),
          wait_timer_(timers_.register_timer("live2d.gpu_wait")),
          publish_timer_(timers_.register_timer("live2d.rgba_publish")),
          rga_timer_(timers_.register_timer("live2d.rga_transform")),
          nv12_publish_timer_(
              timers_.register_timer("live2d.nv12_publish")),
          total_timer_(timers_.register_timer("live2d.render_total")) {
        if (nv12_sink_.context == nullptr || nv12_sink_.submit == nullptr) {
            throw std::invalid_argument(
                "GodotRenderPipeline requires a complete NV12 sink");
        }
        if (config_.render.output_fps == 0) {
            throw std::invalid_argument(
                "Godot render output FPS must be positive");
        }
        if (config_.rga_dma_heap_path.empty()) {
            throw std::invalid_argument(
                "Godot render RGA DMA heap path must not be empty");
        }
        if (config_.metrics_report_interval.count() <= 0) {
            throw std::invalid_argument(
                "Godot render metrics interval must be positive");
        }
    }

    ~Impl() {
        finish_metrics();
        if (godot_) {
            godot_->shutdown();
        }
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void initialize() {
        if (godot_) {
            throw std::logic_error(
                "GodotRenderPipeline can only be initialized once");
        }
        if (finished_) {
            throw std::logic_error(
                "GodotRenderPipeline is already finished");
        }
        if (!config_.metrics_path.empty()) {
            metrics_reporter_ =
                std::make_unique<instrumentation::MetricsJsonlReporter>(
                    timers_,
                    "vtb-live2d.render",
                    config_.metrics_path,
                    config_.metrics_report_interval);
            metrics_reporter_->start();
        }
        godot_ = std::make_unique<LibGodotInstance>(
            passthrough_arguments_, config_.render);
        godot_->start();
    }

    void submit(const Live2DTrackingSnapshot& snapshot) {
        require_godot().submit(snapshot);
    }

    bool run_frame() {
        LibGodotInstance& godot = require_godot();
        auto total_scope = total_timer_.scope();
        {
            auto scope = prepare_timer_.scope();
            godot.prepare_frame(MonotonicClock::now_ns());
        }
        bool iteration_finished = false;
        {
            auto scope = iteration_timer_.scope();
            iteration_finished = godot.iterate();
        }
        {
            auto scope = texture_timer_.scope();
            godot.acquire_viewport_texture();
        }
        {
            auto scope = blit_timer_.scope();
            godot.blit_viewport_texture();
        }
        {
            auto scope = wait_timer_.scope();
            godot.wait_gpu_copy();
        }
        const std::uint64_t ready_timestamp_ns = MonotonicClock::now_ns();
        RgbaFrameView rgba_frame;
        {
            auto scope = publish_timer_.scope();
            rgba_frame = godot.publish_rgba_frame(ready_timestamp_ns);
        }
        Nv12FrameLease nv12 = transform_rgba(rgba_frame);
        {
            auto publish_scope = nv12_publish_timer_.scope();
            if (nv12_sink_.submit(nv12_sink_.context, &nv12) != 0) {
                throw std::runtime_error(
                    "NV12 sink rejected an RGA output frame lease");
            }
        }
        return iteration_finished || godot.should_stop();
    }

    std::uint64_t completed_frames() const noexcept {
        if (!godot_) {
            return 0;
        }
        return godot_->completed_frames();
    }

    void finish() {
        if (finished_) {
            return;
        }
        LibGodotInstance& godot = require_godot();
        std::exception_ptr godot_failure;
        try {
            godot.finish();
        } catch (...) {
            godot_failure = std::current_exception();
        }
        godot_.reset();
        rga_transformer_.finish();
        finish_metrics();
        finished_ = true;
        if (godot_failure) {
            std::rethrow_exception(godot_failure);
        }
    }

    unsigned long iteration_limit() const noexcept {
        return iteration_limit_;
    }

    std::chrono::nanoseconds frame_period() const noexcept {
        return std::chrono::nanoseconds(
            1000000000ULL /
            static_cast<unsigned long>(config_.render.output_fps));
    }

private:
    Nv12FrameLease transform_rgba(const RgbaFrameView& frame) {
        auto rga_scope = rga_timer_.scope();
        return rga_transformer_.transform(frame);
    }

    LibGodotInstance& require_godot() {
        if (!godot_) {
            throw std::logic_error(
                "GodotRenderPipeline is not initialized");
        }
        return *godot_;
    }

    void finish_metrics() noexcept {
        if (!metrics_reporter_ || metrics_finished_) {
            return;
        }
        metrics_reporter_->stop();
        metrics_finished_ = true;
        std::cout << "VTB_METRICS_RESULT source="
                  << metrics_reporter_->source()
                  << " path=" << metrics_reporter_->output_path()
                  << " records=" << metrics_reporter_->record_count()
                  << " failed=";
        if (metrics_reporter_->failed()) {
            std::cout << "1 error=" << metrics_reporter_->last_error();
        } else {
            std::cout << "0";
        }
        std::cout << "\n";
    }

    std::vector<std::string> passthrough_arguments_;
    unsigned long iteration_limit_ = 0;
    GodotRenderPipelineConfig config_;
    RgaNv12FrameTransformer rga_transformer_;
    Nv12FrameSink nv12_sink_;
    instrumentation::TimerRegistry timers_;
    instrumentation::TimerHandle prepare_timer_;
    instrumentation::TimerHandle iteration_timer_;
    instrumentation::TimerHandle texture_timer_;
    instrumentation::TimerHandle blit_timer_;
    instrumentation::TimerHandle wait_timer_;
    instrumentation::TimerHandle publish_timer_;
    instrumentation::TimerHandle rga_timer_;
    instrumentation::TimerHandle nv12_publish_timer_;
    instrumentation::TimerHandle total_timer_;
    std::unique_ptr<instrumentation::MetricsJsonlReporter> metrics_reporter_;
    std::unique_ptr<LibGodotInstance> godot_;
    bool finished_ = false;
    bool metrics_finished_ = false;
};

std::unique_ptr<GodotRenderPipeline> GodotRenderPipeline::create(
    const std::vector<std::string>& passthrough_arguments,
    unsigned long iteration_limit,
    GodotRenderPipelineConfig config,
    Nv12FrameSink nv12_sink) {
    return std::unique_ptr<GodotRenderPipeline>(
        new GodotRenderPipeline(std::make_unique<Impl>(
            passthrough_arguments,
            iteration_limit,
            std::move(config),
            nv12_sink)));
}

GodotRenderPipeline::GodotRenderPipeline(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

GodotRenderPipeline::~GodotRenderPipeline() = default;

void GodotRenderPipeline::initialize() {
    impl_->initialize();
}

void GodotRenderPipeline::submit(
    const Live2DTrackingSnapshot& snapshot) {
    impl_->submit(snapshot);
}

bool GodotRenderPipeline::run_frame() {
    return impl_->run_frame();
}

std::uint64_t GodotRenderPipeline::completed_frames() const noexcept {
    return impl_->completed_frames();
}

void GodotRenderPipeline::finish() {
    impl_->finish();
}

unsigned long GodotRenderPipeline::iteration_limit() const noexcept {
    return impl_->iteration_limit();
}

std::chrono::nanoseconds GodotRenderPipeline::frame_period() const noexcept {
    return impl_->frame_period();
}

}  // namespace vtb
