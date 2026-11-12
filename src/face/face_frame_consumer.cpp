#include "vtb/face/face_frame_consumer.hpp"

#include <stdexcept>
#include <utility>

namespace vtb {

FaceFrameConsumer::FaceFrameConsumer(
    std::unique_ptr<FacePipeline> face_pipeline,
    ResultHandler result_handler)
    : face_pipeline_(std::move(face_pipeline)),
      result_handler_(std::move(result_handler)) {
    if (!face_pipeline_ || !result_handler_) {
        throw std::invalid_argument(
            "FaceFrameConsumer dependencies must not be null");
    }
}

void FaceFrameConsumer::begin_source(VideoStreamGeneration generation) {
    face_pipeline_->begin_source(generation);
}

void FaceFrameConsumer::consume(DecodedFrameLease frame) {
    std::optional<FaceTrackingResult> result =
        face_pipeline_->process(frame.view());
    result_handler_(result);
}

void FaceFrameConsumer::retire_source(
    VideoStreamGeneration generation) noexcept {
    face_pipeline_->retire_source(generation);
}

}  // namespace vtb
