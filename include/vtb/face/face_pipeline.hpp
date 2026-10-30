#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "vtb/face/image_transformer.hpp"

namespace vtb {

class FaceBox {
public:
    FaceBox(float x, float y, float width, float height);
    ~FaceBox() = default;

    FaceBox(const FaceBox&) = default;
    FaceBox& operator=(const FaceBox&) = default;
    FaceBox(FaceBox&&) noexcept = default;
    FaceBox& operator=(FaceBox&&) noexcept = default;

    float x() const noexcept;
    float y() const noexcept;
    float width() const noexcept;
    float height() const noexcept;

private:
    float x_ = 0.0F;
    float y_ = 0.0F;
    float width_ = 0.0F;
    float height_ = 0.0F;
};

class FaceDetection {
public:
    FaceDetection(FaceBox box, float confidence);
    ~FaceDetection() = default;

    FaceDetection(const FaceDetection&) = default;
    FaceDetection& operator=(const FaceDetection&) = default;
    FaceDetection(FaceDetection&&) noexcept = default;
    FaceDetection& operator=(FaceDetection&&) noexcept = default;

    const FaceBox& box() const noexcept;
    float confidence() const noexcept;

private:
    FaceBox box_;
    float confidence_ = 0.0F;
};

class FaceLandmark {
public:
    FaceLandmark(float x, float y, float confidence);
    ~FaceLandmark() = default;

    FaceLandmark(const FaceLandmark&) = default;
    FaceLandmark& operator=(const FaceLandmark&) = default;
    FaceLandmark(FaceLandmark&&) noexcept = default;
    FaceLandmark& operator=(FaceLandmark&&) noexcept = default;

    float x() const noexcept;
    float y() const noexcept;
    float confidence() const noexcept;

private:
    float x_ = 0.0F;
    float y_ = 0.0F;
    float confidence_ = 0.0F;
};

class LandmarkEstimate {
public:
    LandmarkEstimate(
        FaceBox updated_box,
        std::vector<FaceLandmark> landmarks,
        float confidence);
    ~LandmarkEstimate() = default;

    LandmarkEstimate(const LandmarkEstimate&) = default;
    LandmarkEstimate& operator=(const LandmarkEstimate&) = default;
    LandmarkEstimate(LandmarkEstimate&&) noexcept = default;
    LandmarkEstimate& operator=(LandmarkEstimate&&) noexcept = default;

    const FaceBox& updated_box() const noexcept;
    const std::vector<FaceLandmark>& landmarks() const noexcept;
    float confidence() const noexcept;

private:
    FaceBox updated_box_;
    std::vector<FaceLandmark> landmarks_;
    float confidence_ = 0.0F;
};

class FacePose {
public:
    FacePose(float yaw, float pitch, float roll, float pnp_error);
    ~FacePose() = default;

    FacePose(const FacePose&) = default;
    FacePose& operator=(const FacePose&) = default;
    FacePose(FacePose&&) noexcept = default;
    FacePose& operator=(FacePose&&) noexcept = default;

    float yaw() const noexcept;
    float pitch() const noexcept;
    float roll() const noexcept;
    float pnp_error() const noexcept;

private:
    float yaw_ = 0.0F;
    float pitch_ = 0.0F;
    float roll_ = 0.0F;
    float pnp_error_ = 0.0F;
};

class FaceTrackingResult {
public:
    FaceTrackingResult(
        std::uint64_t frame_id,
        std::uint64_t capture_timestamp_ns,
        FaceBox box,
        std::vector<FaceLandmark> landmarks,
        FacePose pose,
        float confidence);
    ~FaceTrackingResult() = default;

    FaceTrackingResult(const FaceTrackingResult&) = default;
    FaceTrackingResult& operator=(const FaceTrackingResult&) = default;
    FaceTrackingResult(FaceTrackingResult&&) noexcept = default;
    FaceTrackingResult& operator=(FaceTrackingResult&&) noexcept = default;

    std::uint64_t frame_id() const noexcept;
    std::uint64_t capture_timestamp_ns() const noexcept;
    const FaceBox& box() const noexcept;
    const std::vector<FaceLandmark>& landmarks() const noexcept;
    const FacePose& pose() const noexcept;
    float confidence() const noexcept;

private:
    std::uint64_t frame_id_ = 0;
    std::uint64_t capture_timestamp_ns_ = 0;
    FaceBox box_;
    std::vector<FaceLandmark> landmarks_;
    FacePose pose_;
    float confidence_ = 0.0F;
};

class FaceDetector {
public:
    FaceDetector() = default;
    virtual ~FaceDetector() = default;

    FaceDetector(const FaceDetector&) = delete;
    FaceDetector& operator=(const FaceDetector&) = delete;
    FaceDetector(FaceDetector&&) = delete;
    FaceDetector& operator=(FaceDetector&&) = delete;

    virtual const ModelInputSpec& input_spec() const noexcept = 0;
    // Face boxes use source-frame pixel coordinates. The implementation uses
    // ModelInputView::source_crop() to invert preprocessing scale and offset.
    virtual std::optional<FaceDetection> detect(
        const ModelInputView& input) = 0;
};

class LandmarkEstimator {
public:
    LandmarkEstimator() = default;
    virtual ~LandmarkEstimator() = default;

    LandmarkEstimator(const LandmarkEstimator&) = delete;
    LandmarkEstimator& operator=(const LandmarkEstimator&) = delete;
    LandmarkEstimator(LandmarkEstimator&&) = delete;
    LandmarkEstimator& operator=(LandmarkEstimator&&) = delete;

    virtual const ModelInputSpec& input_spec() const noexcept = 0;
    // Landmarks and the updated face box use source-frame pixel coordinates.
    virtual LandmarkEstimate estimate(const ModelInputView& input) = 0;
};

class FaceGeometryEstimator {
public:
    FaceGeometryEstimator() = default;
    virtual ~FaceGeometryEstimator() = default;

    FaceGeometryEstimator(const FaceGeometryEstimator&) = delete;
    FaceGeometryEstimator& operator=(const FaceGeometryEstimator&) = delete;
    FaceGeometryEstimator(FaceGeometryEstimator&&) = delete;
    FaceGeometryEstimator& operator=(FaceGeometryEstimator&&) = delete;

    virtual FacePose estimate(
        const FrameView& frame,
        const FaceBox& box,
        const std::vector<FaceLandmark>& landmarks) = 0;
};

class FacePipelineConfig {
public:
    std::size_t redetect_interval_frames = 0;
    float minimum_tracking_confidence = 0.6F;
    float landmark_roi_horizontal_expansion = 0.10F;
    float landmark_roi_vertical_expansion = 0.125F;
};

class FacePipeline {
public:
    FacePipeline(
        FacePipelineConfig config,
        std::unique_ptr<ImageTransformer> image_transformer,
        std::unique_ptr<FaceDetector> detector,
        std::unique_ptr<LandmarkEstimator> landmark_estimator,
        std::unique_ptr<FaceGeometryEstimator> geometry_estimator);
    ~FacePipeline() = default;

    FacePipeline(const FacePipeline&) = delete;
    FacePipeline& operator=(const FacePipeline&) = delete;
    FacePipeline(FacePipeline&&) = delete;
    FacePipeline& operator=(FacePipeline&&) = delete;

    std::optional<FaceTrackingResult> process(const FrameView& frame);

private:
    CropRegion full_frame_crop(const FrameView& frame) const;
    CropRegion face_crop(const FrameView& frame, const FaceBox& box) const;
    void reset_tracking() noexcept;

    FacePipelineConfig config_;
    std::unique_ptr<ImageTransformer> image_transformer_;
    std::unique_ptr<FaceDetector> detector_;
    std::unique_ptr<LandmarkEstimator> landmark_estimator_;
    std::unique_ptr<FaceGeometryEstimator> geometry_estimator_;
    std::optional<FaceBox> tracked_box_;
    float tracking_confidence_ = 0.0F;
    std::size_t frames_since_detection_ = 0;
};

}  // namespace vtb
