#pragma once

#include <cstddef>
#include <memory>

#include "vtb/face/face_pipeline.hpp"
#include "vtb/live2d/live2d_face_mapper.hpp"

namespace vtb {

/**
 * @brief 同步执行人脸处理与 Live2D 映射的已配置任务主体。
 *
 * 构造时接收并固定全部算法依赖和映射配置。该类不创建线程、不持有消息通道；
 * FaceTrackingStage 只负责在工作线程中调用它并完成输入输出交接。
 */
class Live2DTrackingPipeline final {
public:
    /** @brief 依赖注入入口，供测试或替代 FacePipeline 实现使用。 */
    Live2DTrackingPipeline(
        std::unique_ptr<FacePipeline> face_pipeline,
        Live2DMappingConfig mapping_config);
    ~Live2DTrackingPipeline() = default;

    Live2DTrackingPipeline(const Live2DTrackingPipeline&) = delete;
    Live2DTrackingPipeline& operator=(
        const Live2DTrackingPipeline&) = delete;
    Live2DTrackingPipeline(Live2DTrackingPipeline&&) = delete;
    Live2DTrackingPipeline& operator=(Live2DTrackingPipeline&&) = delete;

    void begin_source(VideoStreamGeneration generation);
    Live2DMappingResult process(const FrameView& frame);
    void reset_calibration() noexcept;
    std::size_t calibration_required() const noexcept;
    void retire_source(VideoStreamGeneration generation) noexcept;

private:
    std::unique_ptr<FacePipeline> face_pipeline_;
    Live2DFaceMapper mapper_;
    const std::size_t calibration_required_;
};

}  // namespace vtb
