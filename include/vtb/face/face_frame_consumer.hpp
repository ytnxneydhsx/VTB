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
 * consume() 按值取得 DecodedFrameLease，因此 FacePipeline 同步处理期间源帧一直有效；
 * result_handler_ 只在回调期间借用局部 optional 结果的 const 引用。
 *
 * @htmlonly
 * <div class="vtb-resource-card">
 *   <div class="vtb-resource-card__title">独占处理链与临时帧归还责任</div>
 *   <span class="vtb-resource-owned">face_pipeline_</span> 独占 FacePipeline，
 *   <span class="vtb-resource-owned">result_handler_</span> 按值保存类型擦除回调；
 *   consume() 的 frame 参数在函数期间独占源帧归还责任。析构使用默认实现，复制和
 *   移动均明确删除。
 * </div>
 * <div class="vtb-resource-card vtb-resource-card--borrowed">
 *   <div class="vtb-resource-card__title">结果回调借用窗口</div>
 *   ResultHandler 接收的引用只在 result_handler_(result) 调用期间有效。需要跨线程或
 *   跨回调保存时，处理函数应复制或移动结果值，不能保存该引用。
 * </div>
 * @endhtmlonly
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
