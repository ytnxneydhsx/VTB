#include "pipeline/godot_render_pipeline.hpp"

#include <cstdint>
#include <exception>
#include <stdexcept>
#include <utility>

#include "libgodot_instance.hpp"
#include "vtb/time/monotonic_clock.hpp"
#include "vtb/trace/frame_trace.hpp"

namespace vtb {

class GodotRenderPipeline::Impl final {
public:
    Impl(
        const std::vector<std::string>& passthrough_arguments,
        unsigned long iteration_limit,
        RenderConfiguration config,
        RgbaFrameSink rgba_sink)
        : passthrough_arguments_(passthrough_arguments),
          iteration_limit_(iteration_limit),
          config_(std::move(config)),
          rgba_sink_(rgba_sink) {
        if (rgba_sink_.context == nullptr || rgba_sink_.submit == nullptr) {
            throw std::invalid_argument(
                "GodotRenderPipeline requires a complete RGBA sink");
        }
        if (config_.output_fps == 0) {
            throw std::invalid_argument(
                "Godot render output FPS must be positive");
        }
    }

    ~Impl() {
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
        godot_ = std::make_unique<LibGodotInstance>(
            passthrough_arguments_, config_);
        godot_->start();
    }

    void submit(const Live2DTrackingSnapshot& snapshot) {
        VTB_FRAME_EVENT(
            "render.snapshot_dequeue", frame_trace_track(),
            perfetto::Flow::ProcessScoped(snapshot.snapshot_generation(), "snapshot"),
            "snapshot_generation", snapshot.snapshot_generation(),
            "frame_id", snapshot.source_frame_id());
        require_godot().submit(snapshot);
#ifdef VTB_FRAME_TRACING
        snapshot_generation_ = snapshot.snapshot_generation();
        source_frame_id_ = snapshot.source_frame_id();
        tracking_valid_ = snapshot.mapping().tracking_valid();
        new_snapshot_ = true;
#endif
    }

    FrameResult run_frame() {
        LibGodotInstance& godot = require_godot();
        {
            VTB_FRAME_SLICE("render.prepare", frame_trace_track());
            if (!godot.prepare_frame(MonotonicClock::now_ns())) {
                if (godot.should_stop()) {
                    return FrameResult::finished;
                }
                VTB_FRAME_EVENT("render.rgba_pool_full", frame_trace_track());
                return FrameResult::skipped;
            }
        }
        const bool iteration_finished = [&] {
            VTB_FRAME_SLICE("render.iterate", frame_trace_track());
            return godot.iterate();
        }();
        {
            VTB_FRAME_SLICE("render.wait_gpu", frame_trace_track());
            godot.wait_gpu_render();
        }
        const std::uint64_t ready_timestamp_ns = MonotonicClock::now_ns();
        RgbaFrameLease rgba = godot.publish_rgba_frame(ready_timestamp_ns);
#ifdef VTB_FRAME_TRACING
        if (new_snapshot_) {
            VTB_FRAME_EVENT(
                "render.snapshot_output", frame_trace_track(),
                perfetto::TerminatingFlow::ProcessScoped(snapshot_generation_, "snapshot"),
                "snapshot_generation", snapshot_generation_,
                "frame_id", source_frame_id_);
        }
        VTB_FRAME_EVENT(
            "render.output_ready", frame_trace_track(), "sequence", rgba.view().sequence,
            "snapshot_generation", snapshot_generation_, "frame_id", source_frame_id_,
            "tracking_valid", tracking_valid_, "new_snapshot", new_snapshot_);
        new_snapshot_ = false;
#endif
        if (rgba_sink_.submit(rgba_sink_.context, &rgba) != 0) {
            throw std::runtime_error(
                "RGBA sink rejected a Godot frame lease");
        }
        if (iteration_finished || godot.should_stop()) {
            return FrameResult::finished;
        }
        return FrameResult::rendered;
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
            static_cast<unsigned long>(config_.output_fps));
    }

private:
    LibGodotInstance& require_godot() {
        if (!godot_) {
            throw std::logic_error(
                "GodotRenderPipeline is not initialized");
        }
        return *godot_;
    }

    std::vector<std::string> passthrough_arguments_;
    unsigned long iteration_limit_ = 0;
    RenderConfiguration config_;
    RgbaFrameSink rgba_sink_;
    std::unique_ptr<LibGodotInstance> godot_;
#ifdef VTB_FRAME_TRACING
    std::uint64_t snapshot_generation_ = 0;
    std::uint64_t source_frame_id_ = 0;
    bool tracking_valid_ = false;
    bool new_snapshot_ = false;
#endif
    bool finished_ = false;
};

std::unique_ptr<GodotRenderPipeline> GodotRenderPipeline::create(
    const std::vector<std::string>& passthrough_arguments,
    unsigned long iteration_limit,
    RenderConfiguration config,
    RgbaFrameSink rgba_sink) {
    return std::unique_ptr<GodotRenderPipeline>(
        new GodotRenderPipeline(std::make_unique<Impl>(
            passthrough_arguments,
            iteration_limit,
            std::move(config),
            rgba_sink)));
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

GodotRenderPipeline::FrameResult GodotRenderPipeline::run_frame() {
    return impl_->run_frame();
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
