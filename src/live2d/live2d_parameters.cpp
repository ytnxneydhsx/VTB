#include "vtb/live2d/live2d_parameters.hpp"

#include <cmath>
#include <stdexcept>

namespace vtb {

Live2DParameters::Live2DParameters(
    float angle_x,
    float angle_y,
    float angle_z,
    float body_angle_x,
    float body_angle_y,
    float body_angle_z,
    float left_eye_open,
    float right_eye_open,
    float mouth_open,
    float mouth_form)
    : values_({
          angle_x, angle_y, angle_z,
          body_angle_x, body_angle_y, body_angle_z,
          left_eye_open, right_eye_open,
          mouth_open, mouth_form}) {
    for (float value : values_) {
        if (!std::isfinite(value)) {
            throw std::invalid_argument("Live2D parameter must be finite");
        }
    }
}

float Live2DParameters::angle_x() const noexcept { return values_[0]; }
float Live2DParameters::angle_y() const noexcept { return values_[1]; }
float Live2DParameters::angle_z() const noexcept { return values_[2]; }
float Live2DParameters::body_angle_x() const noexcept { return values_[3]; }
float Live2DParameters::body_angle_y() const noexcept { return values_[4]; }
float Live2DParameters::body_angle_z() const noexcept { return values_[5]; }
float Live2DParameters::left_eye_open() const noexcept { return values_[6]; }
float Live2DParameters::right_eye_open() const noexcept { return values_[7]; }
float Live2DParameters::mouth_open() const noexcept { return values_[8]; }
float Live2DParameters::mouth_form() const noexcept { return values_[9]; }

}  // namespace vtb
