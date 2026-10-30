#pragma once

#include <memory>

#include "vtb/face/face_pipeline.hpp"

namespace vtb {

class OpenSeeFaceGeometryState;

class OpenSeeFaceGeometryEstimator final : public FaceGeometryEstimator {
public:
    OpenSeeFaceGeometryEstimator();
    ~OpenSeeFaceGeometryEstimator() override;

    OpenSeeFaceGeometryEstimator(const OpenSeeFaceGeometryEstimator&) = delete;
    OpenSeeFaceGeometryEstimator& operator=(
        const OpenSeeFaceGeometryEstimator&) = delete;
    OpenSeeFaceGeometryEstimator(OpenSeeFaceGeometryEstimator&&) = delete;
    OpenSeeFaceGeometryEstimator& operator=(
        OpenSeeFaceGeometryEstimator&&) = delete;

    FaceGeometryResult estimate(
        const FrameView& frame,
        const FaceBox& box,
        const std::vector<FaceLandmark>& landmarks) override;
    void reset() noexcept override;

private:
    std::unique_ptr<OpenSeeFaceGeometryState> state_;
};

}  // namespace vtb
