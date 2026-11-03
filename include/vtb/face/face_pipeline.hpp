#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include "vtb/face/face_types.hpp"
#include "vtb/face/image_transformer.hpp"
#include "vtb/face/model_input.hpp"

namespace vtb {

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

    virtual FaceGeometryResult estimate(
        const FrameView& frame,
        const FaceBox& box,
        const std::vector<FaceLandmark>& landmarks) = 0;
    virtual void reset() noexcept = 0;
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
