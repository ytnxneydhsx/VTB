#include "vtb/live2d/live2d_face_mapper.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace vtb {
namespace {

float clamp_value(float value, float lower, float upper) noexcept {
    return std::max(lower, std::min(upper, value));
}

void require_finite(float value, const char* description) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument(description);
    }
}

float normalize_range(float value, float lower, float upper) {
    if (upper <= lower) {
        throw std::invalid_argument(
            "Live2D normalization upper bound must exceed lower bound");
    }
    return clamp_value((value - lower) / (upper - lower), 0.0F, 1.0F);
}

float angle_delta_degrees(float value, float neutral) noexcept {
    float delta = std::fmod(value - neutral + 180.0F, 360.0F);
    if (delta < 0.0F) {
        delta += 360.0F;
    }
    delta -= 180.0F;
    if (delta == -180.0F) {
        return 180.0F;
    }
    return delta;
}

float median(std::vector<float> values) {
    if (values.empty()) {
        throw std::invalid_argument("Median requires at least one value");
    }
    std::sort(values.begin(), values.end());
    const std::size_t middle = values.size() / 2U;
    if ((values.size() & 1U) != 0U) {
        return values[middle];
    }
    return (values[middle - 1U] + values[middle]) * 0.5F;
}

void validate_config(const Live2DMappingConfig& config) {
    if (config.calibration_frames == 0) {
        throw std::invalid_argument("Live2D calibration frames must be positive");
    }
    const std::array<float, 11> values = {
        config.smoothing_tau_ms,
        config.minimum_confidence,
        config.maximum_pnp_error,
        config.angle_limit_degrees,
        config.angle_x_sign,
        config.angle_y_sign,
        config.angle_z_sign,
        config.body_angle_gain,
        config.mouth_closed_value,
        config.mouth_open_value,
        config.mouth_form_gain};
    for (float value : values) {
        require_finite(value, "Live2D mapping configuration must be finite");
    }
    if (config.smoothing_tau_ms < 0.0F ||
        config.minimum_confidence < 0.0F ||
        config.minimum_confidence > 1.0F ||
        config.maximum_pnp_error < 0.0F ||
        config.angle_limit_degrees <= 0.0F ||
        config.body_angle_gain < 0.0F ||
        config.mouth_form_gain < 0.0F ||
        config.mouth_open_value <= config.mouth_closed_value) {
        throw std::invalid_argument("Live2D mapping configuration is invalid");
    }
    const std::array<float, 3> signs = {
        config.angle_x_sign, config.angle_y_sign, config.angle_z_sign};
    for (float sign : signs) {
        if (sign != -1.0F && sign != 1.0F) {
            throw std::invalid_argument("Live2D angle signs must be -1 or 1");
        }
    }
}

}  // namespace

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
        require_finite(value, "Live2D parameter must be finite");
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

Live2DFaceMapper::Live2DFaceMapper(Live2DMappingConfig config)
    : config_(config) {
    validate_config(config_);
    neutral_samples_.reserve(config_.calibration_frames);
}

Live2DMappingResult Live2DFaceMapper::update(
    const FaceTrackingResult* face,
    std::uint64_t timestamp_ns) {
    const bool valid = face_is_valid(face);
    float confidence = 0.0F;
    std::uint64_t frame_id = 0;
    Live2DParameters target;
    if (valid) {
        confidence = face->confidence();
        frame_id = face->frame_id();
        const FacePose& pose = face->pose();
        if (!calibrated_) {
            neutral_samples_.push_back(
                {pose.pitch(), pose.yaw(), pose.roll()});
            if (neutral_samples_.size() >= config_.calibration_frames) {
                finish_calibration();
            }
        }
        if (calibrated_) {
            target = map_valid_face(*face);
        }
    }
    return Live2DMappingResult(
        valid,
        calibrated_,
        calibration_progress(),
        confidence,
        frame_id,
        timestamp_ns,
        smooth(target, timestamp_ns));
}

void Live2DFaceMapper::reset_calibration() noexcept {
    neutral_samples_.clear();
    neutral_euler_ = {0.0F, 0.0F, 0.0F};
    last_parameters_ = Live2DParameters();
    last_timestamp_ns_ = 0;
    calibrated_ = false;
    has_last_timestamp_ = false;
}

bool Live2DFaceMapper::calibrated() const noexcept { return calibrated_; }

std::size_t Live2DFaceMapper::calibration_progress() const noexcept {
    if (calibrated_) {
        return config_.calibration_frames;
    }
    return neutral_samples_.size();
}

bool Live2DFaceMapper::face_is_valid(
    const FaceTrackingResult* face) const noexcept {
    if (face == nullptr || !face->geometry_success()) {
        return false;
    }
    if (!std::isfinite(face->confidence()) ||
        face->confidence() < config_.minimum_confidence) {
        return false;
    }
    const FacePose& pose = face->pose();
    if (!std::isfinite(pose.pitch()) || !std::isfinite(pose.yaw()) ||
        !std::isfinite(pose.roll()) || !std::isfinite(pose.pnp_error()) ||
        pose.pnp_error() > config_.maximum_pnp_error) {
        return false;
    }
    const FaceExpression& expression = face->expression();
    return std::isfinite(expression.left_eye_open()) &&
        std::isfinite(expression.right_eye_open()) &&
        std::isfinite(expression.mouth_open()) &&
        std::isfinite(expression.mouth_wide());
}

void Live2DFaceMapper::finish_calibration() {
    std::vector<float> pitch;
    std::vector<float> yaw;
    std::vector<float> roll;
    pitch.reserve(neutral_samples_.size());
    yaw.reserve(neutral_samples_.size());
    roll.reserve(neutral_samples_.size());
    for (const std::array<float, 3>& sample : neutral_samples_) {
        pitch.push_back(sample[0]);
        yaw.push_back(sample[1]);
        roll.push_back(sample[2]);
    }
    neutral_euler_ = {median(pitch), median(yaw), median(roll)};
    neutral_samples_.clear();
    calibrated_ = true;
}

Live2DParameters Live2DFaceMapper::map_valid_face(
    const FaceTrackingResult& face) const {
    const FacePose& pose = face.pose();
    const float pitch = angle_delta_degrees(pose.pitch(), neutral_euler_[0]);
    const float yaw = angle_delta_degrees(pose.yaw(), neutral_euler_[1]);
    const float roll = angle_delta_degrees(pose.roll(), neutral_euler_[2]);
    const float limit = config_.angle_limit_degrees;
    const float angle_x = clamp_value(
        yaw * config_.angle_x_sign, -limit, limit);
    const float angle_y = clamp_value(
        pitch * config_.angle_y_sign, -limit, limit);
    const float angle_z = clamp_value(
        roll * config_.angle_z_sign, -limit, limit);
    const FaceExpression& expression = face.expression();
    const float mouth_open = normalize_range(
        expression.mouth_open(),
        config_.mouth_closed_value,
        config_.mouth_open_value);
    const float mouth_form = clamp_value(
        expression.mouth_wide() * config_.mouth_form_gain, -1.0F, 1.0F);
    return Live2DParameters(
        angle_x,
        angle_y,
        angle_z,
        angle_x * config_.body_angle_gain,
        angle_y * config_.body_angle_gain,
        angle_z * config_.body_angle_gain,
        clamp_value(expression.left_eye_open(), 0.0F, 1.0F),
        clamp_value(expression.right_eye_open(), 0.0F, 1.0F),
        mouth_open,
        mouth_form);
}

Live2DParameters Live2DFaceMapper::smooth(
    const Live2DParameters& target,
    std::uint64_t timestamp_ns) {
    float alpha = 1.0F;
    if (has_last_timestamp_ && config_.smoothing_tau_ms > 0.0F) {
        std::uint64_t elapsed_ns = 0;
        if (timestamp_ns >= last_timestamp_ns_) {
            elapsed_ns = timestamp_ns - last_timestamp_ns_;
        }
        const double tau_ns =
            static_cast<double>(config_.smoothing_tau_ms) * 1000000.0;
        alpha = static_cast<float>(
            1.0 - std::exp(-static_cast<double>(elapsed_ns) / tau_ns));
    }
    const auto blend = [alpha](float previous, float next) {
        return previous + alpha * (next - previous);
    };
    Live2DParameters result(
        blend(last_parameters_.angle_x(), target.angle_x()),
        blend(last_parameters_.angle_y(), target.angle_y()),
        blend(last_parameters_.angle_z(), target.angle_z()),
        blend(last_parameters_.body_angle_x(), target.body_angle_x()),
        blend(last_parameters_.body_angle_y(), target.body_angle_y()),
        blend(last_parameters_.body_angle_z(), target.body_angle_z()),
        blend(last_parameters_.left_eye_open(), target.left_eye_open()),
        blend(last_parameters_.right_eye_open(), target.right_eye_open()),
        blend(last_parameters_.mouth_open(), target.mouth_open()),
        blend(last_parameters_.mouth_form(), target.mouth_form()));
    last_parameters_ = result;
    last_timestamp_ns_ = timestamp_ns;
    has_last_timestamp_ = true;
    return result;
}

}  // namespace vtb
