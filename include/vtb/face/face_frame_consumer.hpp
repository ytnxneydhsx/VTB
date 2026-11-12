#pragma once

#include <functional>
#include <memory>
#include <optional>

#include "vtb/face/face_pipeline.hpp"
#include "vtb/video/video_frame_consumer.hpp"

namespace vtb {

/**
 * @brief 把通用视频帧消费接口适配到 FacePipeline。
 *
 * 该类是视频运行时与人脸模块之间唯一的适配边界。通用运行时只调用 consume()；
 * FaceFrameConsumer 负责调用 FacePipeline 并把结果交给人脸结果处理函数。
 */
class FaceFrameConsumer final : public VideoFrameConsumer {
public:
    using ResultHandler =
        std::function<void(const std::optional<FaceTrackingResult>&)>;

    FaceFrameConsumer(
        std::unique_ptr<FacePipeline> face_pipeline,
        ResultHandler result_handler);
    ~FaceFrameConsumer() override = default;

    FaceFrameConsumer(const FaceFrameConsumer&) = delete;
    FaceFrameConsumer& operator=(const FaceFrameConsumer&) = delete;
    FaceFrameConsumer(FaceFrameConsumer&&) = delete;
    FaceFrameConsumer& operator=(FaceFrameConsumer&&) = delete;

    void begin_source(VideoStreamGeneration generation) override;
    void consume(DecodedFrameLease frame) override;
    void retire_source(VideoStreamGeneration generation) noexcept override;

private:
    std::unique_ptr<FacePipeline> face_pipeline_;
    ResultHandler result_handler_;
};

}  // namespace vtb
