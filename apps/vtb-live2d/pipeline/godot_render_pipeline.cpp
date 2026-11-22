#include "pipeline/godot_render_pipeline.hpp"

#include <cstdint>
#include <exception>
#include <stdexcept>
#include <utility>

#include "libgodot_instance.hpp"
#include "vtb/hardware/rga/rga_nv12_frame_transformer.hpp"
#include "vtb/time/monotonic_clock.hpp"
#include "vtb/trace/frame_trace.hpp"

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
          nv12_sink_(nv12_sink) {
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
            passthrough_arguments_, config_.render);
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

    bool run_frame() {
        LibGodotInstance& godot = require_godot();
        {
            VTB_FRAME_SLICE("render.prepare", frame_trace_track());
            godot.prepare_frame(MonotonicClock::now_ns());
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
        const RgbaFrameView rgba_frame =
            godot.publish_rgba_frame(ready_timestamp_ns);
        VTB_FRAME_EVENT(
            "render.rgba_ready", frame_trace_track(), "sequence", rgba_frame.sequence);
        Nv12FrameLease nv12 = [&] {
            VTB_FRAME_SLICE("rga.nv12_convert", frame_trace_track());
            return rga_transformer_.transform(rgba_frame);
        }();
#ifdef VTB_FRAME_TRACING
        if (new_snapshot_) {
            VTB_FRAME_EVENT(
                "render.snapshot_output", frame_trace_track(),
                perfetto::TerminatingFlow::ProcessScoped(snapshot_generation_, "snapshot"),
                "snapshot_generation", snapshot_generation_,
                "frame_id", source_frame_id_);
        }
        VTB_FRAME_EVENT(
            "render.nv12_ready", frame_trace_track(), "sequence", nv12.view().sequence,
            "snapshot_generation", snapshot_generation_, "frame_id", source_frame_id_,
            "tracking_valid", tracking_valid_, "new_snapshot", new_snapshot_);
        new_snapshot_ = false;
#endif
        if (nv12_sink_.submit(nv12_sink_.context, &nv12) != 0) {
            throw std::runtime_error(
                "NV12 sink rejected an RGA output frame lease");
        }
        return iteration_finished || godot.should_stop();
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
    LibGodotInstance& require_godot() {
        if (!godot_) {
            throw std::logic_error(
                "GodotRenderPipeline is not initialized");
        }
        return *godot_;
    }

    std::vector<std::string> passthrough_arguments_;
    unsigned long iteration_limit_ = 0;
    GodotRenderPipelineConfig config_;
    RgaNv12FrameTransformer rga_transformer_;
    Nv12FrameSink nv12_sink_;
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
