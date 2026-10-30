#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "vtb/face/face_pipeline.hpp"

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

class Live2DParameters {
public:
    Live2DParameters() = default;
    Live2DParameters(
        float angle_x,
        float angle_y,
        float angle_z,
        float body_angle_x,
        float body_angle_y,
        float body_angle_z,
        float left_eye_open,
        float right_eye_open,
        float mouth_open,
        float mouth_form);
    ~Live2DParameters() = default;

    Live2DParameters(const Live2DParameters&) = default;
    Live2DParameters& operator=(const Live2DParameters&) = default;
    Live2DParameters(Live2DParameters&&) noexcept = default;
    Live2DParameters& operator=(Live2DParameters&&) noexcept = default;

    float angle_x() const noexcept;
    float angle_y() const noexcept;
    float angle_z() const noexcept;
    float body_angle_x() const noexcept;
    float body_angle_y() const noexcept;
    float body_angle_z() const noexcept;
    float left_eye_open() const noexcept;
    float right_eye_open() const noexcept;
    float mouth_open() const noexcept;
    float mouth_form() const noexcept;

private:
    std::array<float, 10> values_ = {
        0.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 0.0F,
        1.0F, 1.0F,
        0.0F, 0.0F};
};

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
