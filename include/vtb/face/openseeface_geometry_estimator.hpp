#pragma once

#include <memory>

#include "vtb/face/face_pipeline.hpp"

namespace vtb {

class OpenSeeFaceGeometryState;

/**
 * @brief 使用 OpenCV PnP 和 OpenSeeFace 兼容特征计算姿态与眼睛、嘴部表情。
 *
 * state_ 保存上一帧外参、PnP 异常计数、特征平滑器和最近有效表情。estimate() 同步
 * 借用源帧尺寸和 66 点，不保存 FrameView 或关键点引用；reset() 在跟踪丢失时清除
 * 全部跨帧状态。
 *
 * @htmlonly
 * <div class="vtb-resource-card">
 *   <div class="vtb-resource-card__title">独占几何状态</div>
 *   <span class="vtb-resource-owned">state_</span> 独占 OpenSeeFaceGeometryState 以及其中
 *   的 cv::Mat 和平滑状态。析构在实现文件定义，复制和移动均明确删除。
 * </div>
 * <div class="vtb-resource-card vtb-resource-card--borrowed">
 *   <div class="vtb-resource-card__title">同步借用输入</div>
 *   frame、box 和 landmarks 只在 estimate() 调用期间被读取；析构不会处理源帧、
 *   人脸框或关键点存储。
 * </div>
 * @endhtmlonly
 */
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
