#include "vtb/live2d/live2d_mapping_result.hpp"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace vtb {

Live2DMappingResult::Live2DMappingResult(
    bool tracking_valid,
    bool calibrated,
    std::size_t calibration_progress,
    float confidence,
    std::uint64_t frame_id,
    std::uint64_t produced_timestamp_ns,
    Live2DParameters parameters)
    : tracking_valid_(tracking_valid),
      calibrated_(calibrated),
      calibration_progress_(calibration_progress),
      confidence_(confidence),
      frame_id_(frame_id),
      produced_timestamp_ns_(produced_timestamp_ns),
      parameters_(std::move(parameters)) {
    if (!std::isfinite(confidence_) || confidence_ < 0.0F ||
        confidence_ > 1.0F) {
        throw std::invalid_argument(
            "Live2D mapping confidence must be within [0, 1]");
    }
}

bool Live2DMappingResult::tracking_valid() const noexcept {
    return tracking_valid_;
}

bool Live2DMappingResult::calibrated() const noexcept { return calibrated_; }

std::size_t Live2DMappingResult::calibration_progress() const noexcept {
    return calibration_progress_;
}

float Live2DMappingResult::confidence() const noexcept { return confidence_; }
std::uint64_t Live2DMappingResult::frame_id() const noexcept { return frame_id_; }

std::uint64_t Live2DMappingResult::produced_timestamp_ns() const noexcept {
    return produced_timestamp_ns_;
}

const Live2DParameters& Live2DMappingResult::parameters() const noexcept {
    return parameters_;
}

}  // namespace vtb
