#include "vtb/live2d/live2d_parameter_interpolator.hpp"

#include <limits>

namespace vtb {
namespace {

float interpolate(float start, float end, float alpha) {
    return start + alpha * (end - start);
}

Live2DParameters interpolate_parameters(
    const Live2DParameters& start,
    const Live2DParameters& end,
    float alpha) {
    return Live2DParameters(
        interpolate(start.angle_x(), end.angle_x(), alpha),
        interpolate(start.angle_y(), end.angle_y(), alpha),
        interpolate(start.angle_z(), end.angle_z(), alpha),
        interpolate(start.body_angle_x(), end.body_angle_x(), alpha),
        interpolate(start.body_angle_y(), end.body_angle_y(), alpha),
        interpolate(start.body_angle_z(), end.body_angle_z(), alpha),
        interpolate(start.left_eye_open(), end.left_eye_open(), alpha),
        interpolate(start.right_eye_open(), end.right_eye_open(), alpha),
        interpolate(start.mouth_open(), end.mouth_open(), alpha),
        interpolate(start.mouth_form(), end.mouth_form(), alpha));
}

}  // namespace

void Live2DParameterInterpolator::reset() noexcept {
    previous_parameters_ = Live2DParameters();
    current_parameters_ = Live2DParameters();
    previous_timestamp_ns_ = 0;
    current_timestamp_ns_ = 0;
    has_previous_ = false;
    has_current_ = false;
}

bool Live2DParameterInterpolator::push(
    const Live2DParameters& parameters,
    std::uint64_t produced_timestamp_ns) noexcept {
    if (produced_timestamp_ns == 0) {
        return false;
    }
    if (has_current_ && produced_timestamp_ns <= current_timestamp_ns_) {
        return false;
    }
    if (has_current_) {
        previous_parameters_ = current_parameters_;
        previous_timestamp_ns_ = current_timestamp_ns_;
        has_previous_ = true;
    }
    current_parameters_ = parameters;
    current_timestamp_ns_ = produced_timestamp_ns;
    has_current_ = true;
    return true;
}

bool Live2DParameterInterpolator::has_parameters() const noexcept {
    return has_current_;
}

Live2DParameters Live2DParameterInterpolator::sample(
    std::uint64_t render_timestamp_ns,
    std::uint64_t render_interval_ns) const {
    if (!has_current_) {
        return Live2DParameters();
    }
    if (!has_previous_ || render_interval_ns == 0) {
        return current_parameters_;
    }

    const std::uint64_t source_interval_ns =
        current_timestamp_ns_ - previous_timestamp_ns_;
    std::uint64_t direct_update_threshold_ns =
        std::numeric_limits<std::uint64_t>::max();
    if (render_interval_ns <=
        std::numeric_limits<std::uint64_t>::max() - render_interval_ns / 2) {
        direct_update_threshold_ns =
            render_interval_ns + render_interval_ns / 2;
    }
    if (source_interval_ns <= direct_update_threshold_ns) {
        return current_parameters_;
    }

    std::uint64_t target_timestamp_ns = 0;
    if (render_timestamp_ns > source_interval_ns) {
        target_timestamp_ns = render_timestamp_ns - source_interval_ns;
    }
    if (target_timestamp_ns <= previous_timestamp_ns_) {
        return previous_parameters_;
    }
    if (target_timestamp_ns >= current_timestamp_ns_) {
        return current_parameters_;
    }

    const double elapsed_ns = static_cast<double>(
        target_timestamp_ns - previous_timestamp_ns_);
    const float alpha = static_cast<float>(
        elapsed_ns / static_cast<double>(source_interval_ns));
    return interpolate_parameters(
        previous_parameters_, current_parameters_, alpha);
}

}  // namespace vtb
