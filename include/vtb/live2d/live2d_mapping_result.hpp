#pragma once

#include <cstddef>
#include <cstdint>

#include "vtb/live2d/live2d_parameters.hpp"

namespace vtb {

class Live2DMappingResult {
public:
    Live2DMappingResult(
        bool tracking_valid,
        bool calibrated,
        std::size_t calibration_progress,
        float confidence,
        std::uint64_t frame_id,
        std::uint64_t produced_timestamp_ns,
        Live2DParameters parameters);
    ~Live2DMappingResult() = default;

    Live2DMappingResult(const Live2DMappingResult&) = default;
    Live2DMappingResult& operator=(const Live2DMappingResult&) = default;
    Live2DMappingResult(Live2DMappingResult&&) noexcept = default;
    Live2DMappingResult& operator=(Live2DMappingResult&&) noexcept = default;

    bool tracking_valid() const noexcept;
    bool calibrated() const noexcept;
    std::size_t calibration_progress() const noexcept;
    float confidence() const noexcept;
    std::uint64_t frame_id() const noexcept;
    std::uint64_t produced_timestamp_ns() const noexcept;
    const Live2DParameters& parameters() const noexcept;

private:
    bool tracking_valid_ = false;
    bool calibrated_ = false;
    std::size_t calibration_progress_ = 0;
    float confidence_ = 0.0F;
    std::uint64_t frame_id_ = 0;
    std::uint64_t produced_timestamp_ns_ = 0;
    Live2DParameters parameters_;
};

}  // namespace vtb
