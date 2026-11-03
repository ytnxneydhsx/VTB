#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "vtb/face/face_types.hpp"
#include "vtb/live2d/live2d_mapping_result.hpp"

namespace vtb {

class Live2DMappingConfig {
public:
    std::size_t calibration_frames = 30;
    float smoothing_tau_ms = 50.0F;
    float minimum_confidence = 0.4F;
    float maximum_pnp_error = 100.0F;
    float angle_limit_degrees = 30.0F;
    float angle_x_sign = 1.0F;
    float angle_y_sign = 1.0F;
    float angle_z_sign = 1.0F;
    float body_angle_gain = 0.3F;
    float mouth_closed_value = 0.0F;
    float mouth_open_value = 1.0F;
    float mouth_form_gain = 1.0F;
};

class Live2DFaceMapper {
public:
    explicit Live2DFaceMapper(Live2DMappingConfig config);
    ~Live2DFaceMapper() = default;

    Live2DFaceMapper(const Live2DFaceMapper&) = delete;
    Live2DFaceMapper& operator=(const Live2DFaceMapper&) = delete;
    Live2DFaceMapper(Live2DFaceMapper&&) = delete;
    Live2DFaceMapper& operator=(Live2DFaceMapper&&) = delete;

    Live2DMappingResult update(
        const FaceTrackingResult* face,
        std::uint64_t timestamp_ns);
    void reset_calibration() noexcept;
    bool calibrated() const noexcept;
    std::size_t calibration_progress() const noexcept;

private:
    bool face_is_valid(const FaceTrackingResult* face) const noexcept;
    void finish_calibration();
    Live2DParameters map_valid_face(const FaceTrackingResult& face) const;
    Live2DParameters smooth(
        const Live2DParameters& target,
        std::uint64_t timestamp_ns);

    Live2DMappingConfig config_;
    std::vector<std::array<float, 3>> neutral_samples_;
    std::array<float, 3> neutral_euler_ = {0.0F, 0.0F, 0.0F};
    Live2DParameters last_parameters_;
    std::uint64_t last_timestamp_ns_ = 0;
    bool calibrated_ = false;
    bool has_last_timestamp_ = false;
};

}  // namespace vtb
