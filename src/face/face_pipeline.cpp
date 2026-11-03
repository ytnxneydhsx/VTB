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
    FaceGeometryResult geometry = geometry_estimator_->estimate(
        frame, *tracked_box_, landmarks.landmarks());
    return FaceTrackingResult(
        frame.frame_id(),
        frame.timestamp_ns(),
        *tracked_box_,
        landmarks.landmarks(),
        geometry.pose(),
        geometry.expression(),
        geometry.success(),
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
    geometry_estimator_->reset();
}

}  // namespace vtb
