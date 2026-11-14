#include "pipeline/live2d_tracking_pipeline.hpp"

#include <optional>
#include <stdexcept>
#include <utility>

#include "vtb/time/monotonic_clock.hpp"

namespace vtb {

Live2DTrackingPipeline::Live2DTrackingPipeline(
    std::unique_ptr<FacePipeline> face_pipeline,
    Live2DMappingConfig mapping_config)
    : face_pipeline_(std::move(face_pipeline)),
      mapper_(mapping_config),
      calibration_required_(mapping_config.calibration_frames) {
    if (!face_pipeline_) {
        throw std::invalid_argument(
            "Live2DTrackingPipeline face pipeline must not be null");
    }
}

void Live2DTrackingPipeline::begin_source(
    VideoStreamGeneration generation) {
    face_pipeline_->begin_source(generation);
}

Live2DMappingResult Live2DTrackingPipeline::process(
    const FrameView& frame) {
    std::optional<FaceTrackingResult> face =
        face_pipeline_->process(frame);
    const FaceTrackingResult* face_value = nullptr;
    if (face.has_value()) {
        face_value = &face.value();
    }
    return mapper_.update(face_value, MonotonicClock::now_ns());
}

void Live2DTrackingPipeline::reset_calibration() noexcept {
    mapper_.reset_calibration();
}

std::size_t Live2DTrackingPipeline::calibration_required() const noexcept {
    return calibration_required_;
}

void Live2DTrackingPipeline::retire_source(
    VideoStreamGeneration generation) noexcept {
    face_pipeline_->retire_source(generation);
}

}  // namespace vtb
