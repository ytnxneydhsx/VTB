#include "vtb/face/face_pipeline.hpp"

#include <algorithm>
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

FaceTrackingResult::FaceTrackingResult(
    std::uint64_t frame_id,
    std::uint64_t capture_timestamp_ns,
    FaceBox box,
    std::vector<FaceLandmark> landmarks,
    FacePose pose,
    float confidence)
    : frame_id_(frame_id),
      capture_timestamp_ns_(capture_timestamp_ns),
      box_(std::move(box)),
      landmarks_(std::move(landmarks)),
      pose_(std::move(pose)),
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
float FaceTrackingResult::confidence() const noexcept { return confidence_; }

FacePipeline::FacePipeline(
    FacePipelineConfig config,
    std::unique_ptr<ImageTransformer> image_transformer,
    std::unique_ptr<FaceDetector> detector,
    std::unique_ptr<LandmarkEstimator> landmark_estimator,
    std::unique_ptr<FaceGeometryEstimator> geometry_estimator)
    : config_(config),
      image_transformer_(std::move(image_transformer)),
      detector_(std::move(detector)),
      landmark_estimator_(std::move(landmark_estimator)),
      geometry_estimator_(std::move(geometry_estimator)) {
    if (!image_transformer_ || !detector_ || !landmark_estimator_ ||
        !geometry_estimator_) {
        throw std::invalid_argument("FacePipeline dependencies must not be null");
    }
    require_unit_interval(
        config_.minimum_tracking_confidence,
        "Minimum tracking confidence must be within [0, 1]");
    require_finite(
        config_.landmark_roi_horizontal_expansion,
        "Landmark ROI horizontal expansion must be finite");
    require_finite(
        config_.landmark_roi_vertical_expansion,
        "Landmark ROI vertical expansion must be finite");
    if (config_.landmark_roi_horizontal_expansion < 0.0F ||
        config_.landmark_roi_vertical_expansion < 0.0F) {
        throw std::invalid_argument(
            "Landmark ROI expansion values must be non-negative");
    }
}

std::optional<FaceTrackingResult> FacePipeline::process(const FrameView& frame) {
    bool must_detect = !tracked_box_.has_value();
    if (tracking_confidence_ < config_.minimum_tracking_confidence) {
        must_detect = true;
    }
    if (config_.redetect_interval_frames > 0 &&
        frames_since_detection_ >= config_.redetect_interval_frames) {
        must_detect = true;
    }

    if (must_detect) {
        ModelInputLease detector_input = image_transformer_->prepare(
            frame, full_frame_crop(frame), detector_->input_spec());
        std::optional<FaceDetection> detection =
            detector_->detect(detector_input.view());
        if (!detection ||
            detection->confidence() < config_.minimum_tracking_confidence) {
            reset_tracking();
            return std::nullopt;
        }
        tracked_box_ = detection->box();
        tracking_confidence_ = detection->confidence();
        frames_since_detection_ = 0;
    }

    ModelInputLease landmark_input = image_transformer_->prepare(
        frame,
        face_crop(frame, *tracked_box_),
        landmark_estimator_->input_spec());
    LandmarkEstimate landmarks =
        landmark_estimator_->estimate(landmark_input.view());
    if (landmarks.confidence() <= config_.minimum_tracking_confidence) {
        reset_tracking();
        return std::nullopt;
    }

    tracked_box_ = landmarks.updated_box();
    tracking_confidence_ = landmarks.confidence();
    ++frames_since_detection_;
    FacePose pose = geometry_estimator_->estimate(
        frame, *tracked_box_, landmarks.landmarks());
    return FaceTrackingResult(
        frame.frame_id(),
        frame.timestamp_ns(),
        *tracked_box_,
        landmarks.landmarks(),
        std::move(pose),
        tracking_confidence_);
}

CropRegion FacePipeline::full_frame_crop(const FrameView& frame) const {
    return CropRegion(0, 0, frame.width(), frame.height());
}

CropRegion FacePipeline::face_crop(
    const FrameView& frame,
    const FaceBox& box) const {
    const float frame_width = static_cast<float>(frame.width());
    const float frame_height = static_cast<float>(frame.height());
    const int horizontal_margin = static_cast<int>(
        box.width() * config_.landmark_roi_horizontal_expansion);
    const int vertical_margin = static_cast<int>(
        box.height() * config_.landmark_roi_vertical_expansion);
    float left_value = box.x() - static_cast<float>(horizontal_margin);
    float top_value = box.y() - static_cast<float>(vertical_margin);
    float right_value =
        box.x() + box.width() + static_cast<float>(horizontal_margin);
    float bottom_value =
        box.y() + box.height() + static_cast<float>(vertical_margin);
    left_value = std::max(0.0F, std::min(frame_width - 1.0F, left_value));
    right_value = std::max(0.0F, std::min(frame_width - 1.0F, right_value));
    top_value = std::max(0.0F, std::min(frame_height - 1.0F, top_value));
    bottom_value = std::max(0.0F, std::min(frame_height - 1.0F, bottom_value));
    const std::uint32_t left = static_cast<std::uint32_t>(left_value);
    const std::uint32_t right = static_cast<std::uint32_t>(right_value);
    const std::uint32_t top =
        static_cast<std::uint32_t>(top_value + 1.0F);
    const std::uint32_t bottom =
        static_cast<std::uint32_t>(bottom_value + 1.0F);
    if (right <= left || bottom <= top) {
        throw std::runtime_error("Tracked face box is outside the source frame");
    }
    return CropRegion(
        left, top, right - left, bottom - top);
}

void FacePipeline::reset_tracking() noexcept {
    tracked_box_.reset();
    tracking_confidence_ = 0.0F;
    frames_since_detection_ = 0;
}

}  // namespace vtb
