#include "vtb/face/face_types.hpp"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace vtb {
namespace {

void require_finite(float value, const char* description) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument(description);
    }
}

void require_unit_interval(float value, const char* description) {
    require_finite(value, description);
    if (value < 0.0F || value > 1.0F) {
        throw std::invalid_argument(description);
    }
}

}  // namespace

FaceBox::FaceBox(float x, float y, float width, float height)
    : x_(x), y_(y), width_(width), height_(height) {
    require_finite(x_, "Face box x must be finite");
    require_finite(y_, "Face box y must be finite");
    require_finite(width_, "Face box width must be finite");
    require_finite(height_, "Face box height must be finite");
    if (width_ <= 0.0F || height_ <= 0.0F) {
        throw std::invalid_argument("Face box dimensions must be positive");
    }
}

float FaceBox::x() const noexcept { return x_; }
float FaceBox::y() const noexcept { return y_; }
float FaceBox::width() const noexcept { return width_; }
float FaceBox::height() const noexcept { return height_; }

FaceDetection::FaceDetection(FaceBox box, float confidence)
    : box_(std::move(box)), confidence_(confidence) {
    require_unit_interval(
        confidence_, "Face detection confidence must be within [0, 1]");
}

const FaceBox& FaceDetection::box() const noexcept { return box_; }
float FaceDetection::confidence() const noexcept { return confidence_; }

FaceLandmark::FaceLandmark(float x, float y, float confidence)
    : x_(x), y_(y), confidence_(confidence) {
    require_finite(x_, "Face landmark x must be finite");
    require_finite(y_, "Face landmark y must be finite");
    require_unit_interval(
        confidence_, "Face landmark confidence must be within [0, 1]");
}

float FaceLandmark::x() const noexcept { return x_; }
float FaceLandmark::y() const noexcept { return y_; }
float FaceLandmark::confidence() const noexcept { return confidence_; }

LandmarkEstimate::LandmarkEstimate(
    FaceBox updated_box,
    std::vector<FaceLandmark> landmarks,
    float confidence)
    : updated_box_(std::move(updated_box)),
      landmarks_(std::move(landmarks)),
      confidence_(confidence) {
    if (landmarks_.size() != 66) {
        throw std::invalid_argument("Landmark estimator must return 66 points");
    }
    require_unit_interval(
        confidence_, "Landmark confidence must be within [0, 1]");
}

const FaceBox& LandmarkEstimate::updated_box() const noexcept {
    return updated_box_;
}

const std::vector<FaceLandmark>& LandmarkEstimate::landmarks() const noexcept {
    return landmarks_;
}

float LandmarkEstimate::confidence() const noexcept { return confidence_; }

FacePose::FacePose(float yaw, float pitch, float roll, float pnp_error)
    : yaw_(yaw), pitch_(pitch), roll_(roll), pnp_error_(pnp_error) {
    require_finite(yaw_, "Face pose yaw must be finite");
    require_finite(pitch_, "Face pose pitch must be finite");
    require_finite(roll_, "Face pose roll must be finite");
    require_finite(pnp_error_, "Face pose PnP error must be finite");
    if (pnp_error_ < 0.0F) {
        throw std::invalid_argument("Face pose PnP error must be non-negative");
    }
}

float FacePose::yaw() const noexcept { return yaw_; }
float FacePose::pitch() const noexcept { return pitch_; }
float FacePose::roll() const noexcept { return roll_; }
float FacePose::pnp_error() const noexcept { return pnp_error_; }

FaceExpression::FaceExpression(
    float left_eye_open,
    float right_eye_open,
    float mouth_open,
    float mouth_wide)
    : left_eye_open_(left_eye_open),
      right_eye_open_(right_eye_open),
      mouth_open_(mouth_open),
      mouth_wide_(mouth_wide) {
    require_unit_interval(
        left_eye_open_, "Left eye openness must be within [0, 1]");
    require_unit_interval(
        right_eye_open_, "Right eye openness must be within [0, 1]");
    require_finite(mouth_open_, "Mouth openness must be finite");
    require_finite(mouth_wide_, "Mouth width must be finite");
}

float FaceExpression::left_eye_open() const noexcept {
    return left_eye_open_;
}

float FaceExpression::right_eye_open() const noexcept {
    return right_eye_open_;
}

float FaceExpression::mouth_open() const noexcept { return mouth_open_; }
float FaceExpression::mouth_wide() const noexcept { return mouth_wide_; }

FaceGeometryResult::FaceGeometryResult(
    bool success,
    FacePose pose,
    FaceExpression expression)
    : success_(success),
      pose_(std::move(pose)),
      expression_(std::move(expression)) {}

bool FaceGeometryResult::success() const noexcept { return success_; }
const FacePose& FaceGeometryResult::pose() const noexcept { return pose_; }

const FaceExpression& FaceGeometryResult::expression() const noexcept {
    return expression_;
}

FaceTrackingResult::FaceTrackingResult(
    std::uint64_t frame_id,
    std::uint64_t capture_timestamp_ns,
    FaceBox box,
    std::vector<FaceLandmark> landmarks,
    FacePose pose,
    FaceExpression expression,
    bool geometry_success,
    float confidence)
    : frame_id_(frame_id),
      capture_timestamp_ns_(capture_timestamp_ns),
      box_(std::move(box)),
      landmarks_(std::move(landmarks)),
      pose_(std::move(pose)),
      expression_(std::move(expression)),
      geometry_success_(geometry_success),
      confidence_(confidence) {
    if (landmarks_.size() != 66) {
        throw std::invalid_argument("Face tracking result must contain 66 points");
    }
    require_unit_interval(
        confidence_, "Face tracking confidence must be within [0, 1]");
}

std::uint64_t FaceTrackingResult::frame_id() const noexcept { return frame_id_; }

std::uint64_t FaceTrackingResult::capture_timestamp_ns() const noexcept {
    return capture_timestamp_ns_;
}

const FaceBox& FaceTrackingResult::box() const noexcept { return box_; }

const std::vector<FaceLandmark>& FaceTrackingResult::landmarks() const noexcept {
    return landmarks_;
}

const FacePose& FaceTrackingResult::pose() const noexcept { return pose_; }

const FaceExpression& FaceTrackingResult::expression() const noexcept {
    return expression_;
}

bool FaceTrackingResult::geometry_success() const noexcept {
    return geometry_success_;
}

float FaceTrackingResult::confidence() const noexcept { return confidence_; }

}  // namespace vtb
