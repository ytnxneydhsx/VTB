#pragma once

#include <memory>
#include <string>

#include "vtb/face/face_pipeline.hpp"

namespace vtb {

enum class RknnCore {
    core0,
    core1,
    core2,
    automatic,
};

class RknnModel;

class RknnFaceDetector final : public FaceDetector {
public:
    RknnFaceDetector(
        std::string model_path,
        RknnCore core = RknnCore::core0,
        float confidence_threshold = 0.6F);
    ~RknnFaceDetector() override;

    RknnFaceDetector(const RknnFaceDetector&) = delete;
    RknnFaceDetector& operator=(const RknnFaceDetector&) = delete;
    RknnFaceDetector(RknnFaceDetector&&) = delete;
    RknnFaceDetector& operator=(RknnFaceDetector&&) = delete;

    const ModelInputSpec& input_spec() const noexcept override;
    std::optional<FaceDetection> detect(
        const ModelInputView& input) override;

private:
    std::unique_ptr<RknnModel> model_;
    ModelInputSpec input_spec_;
    float confidence_threshold_ = 0.6F;
};

class RknnLandmarkEstimator final : public LandmarkEstimator {
public:
    RknnLandmarkEstimator(
        std::string model_path,
        RknnCore core = RknnCore::core1);
    ~RknnLandmarkEstimator() override;

    RknnLandmarkEstimator(const RknnLandmarkEstimator&) = delete;
    RknnLandmarkEstimator& operator=(const RknnLandmarkEstimator&) = delete;
    RknnLandmarkEstimator(RknnLandmarkEstimator&&) = delete;
    RknnLandmarkEstimator& operator=(RknnLandmarkEstimator&&) = delete;

    const ModelInputSpec& input_spec() const noexcept override;
    LandmarkEstimate estimate(const ModelInputView& input) override;

private:
    std::unique_ptr<RknnModel> model_;
    ModelInputSpec input_spec_;
};

}  // namespace vtb
