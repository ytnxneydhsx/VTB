#pragma once

#include <array>

namespace vtb {

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

}  // namespace vtb
