#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include "vtb/face/face_types.hpp"
#include "vtb/face/image_transformer.hpp"
#include "vtb/face/model_input.hpp"

namespace vtb {

/**
 * @brief 把模型输入转换为源帧坐标人脸框的检测接口。
 *
 * detect() 只在调用期间借用 ModelInputView。实现必须使用 source_crop() 把模型
 * 坐标还原到源帧像素坐标，不能让视图、地址或 fd 逃逸到调用结束之后。
 */
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

/**
 * @brief 从人脸 ROI 模型输入生成 66 点、更新后人脸框和平均置信度的接口。
 *
 * estimate() 只在调用期间借用 ModelInputView，返回值全部使用源帧像素坐标。
 */
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

/**
 * @brief 从源帧尺寸和 66 点计算头部姿态与表情的有状态接口。
 *
 * estimate() 只在同步调用期间借用 FrameView、FaceBox 和关键点集合；reset()
 * 用于跟踪丢失时清除跨帧几何与表情状态。
 */
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

/**
 * @brief 协调按需人脸检测、逐帧关键点、几何计算和跟踪状态更新。
 *
 * process() 同步借用调用方仍由 DecodedFrameLease 保证有效的 FrameView。没有历史框、
 * 置信度低于阈值或达到非零重检间隔时先执行整帧检测；其余帧使用上一帧关键点模型
 * 返回的 updated_box()。方法返回前完成全部 RGA、RKNN 和几何调用，不保存源帧视图。
 *
 * @htmlonly
 * <div class="vtb-resource-card">
 *   <div class="vtb-resource-card__title">独占算法依赖与跨帧状态</div>
 *   产品创建入口在模块内部构造依赖；<span class="vtb-resource-owned">image_transformer_、detector_、
 *   landmark_estimator_、geometry_estimator_</span> 由四个 unique_ptr 独占；
 *   tracked_box_、tracking_confidence_ 和 frames_since_detection_ 保存跟踪状态。
 *   析构使用默认实现并按成员逆序释放依赖，复制和移动均明确删除。
 * </div>
 * <div class="vtb-resource-card vtb-resource-card--borrowed">
 *   <div class="vtb-resource-card__title">同步借用源帧</div>
 *   process() 的 <span class="vtb-resource-borrowed">frame</span> 只在本次调用期间有效；
 *   它的像素地址和 DMA-BUF fd 由上游 DecodedFrameLease 保持有效。
 * </div>
 * @endhtmlonly
 */
class FacePipeline {
public:
    /**
     * @brief 根据完整的人脸流水线配置创建 RK3588 产品实现。
     *
     * 该入口在 FacePipeline 模块内部装配 RGA、RKNN 与几何估计器；调用方不需要
     * 了解模型 core 分配、Transformer 类型或硬件资源的所有权。
     */
    static std::unique_ptr<FacePipeline> create(FacePipelineConfig config);

    /** @brief 依赖注入入口，供独立组件测试和替代后端使用。 */
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

    void begin_source(VideoStreamGeneration generation);
    std::optional<FaceTrackingResult> process(const FrameView& frame);
    void retire_source(VideoStreamGeneration generation) noexcept;

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
