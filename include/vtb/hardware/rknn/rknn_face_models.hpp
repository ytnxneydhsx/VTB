#pragma once

// RKNN-backed face model sessions keep their runtime contexts private.

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

/**
 * @brief 在 RKNN Runtime 上执行 OpenSeeFace 人脸检测并还原源帧坐标人脸框。
 *
 * 模型输出契约为两个 2x56x56 张量：第一个的通道 0 是置信度，通道 1 是
 * 归一化半径；第二个的通道 0 是 max-pool 峰值。detect() 选取最高可信局部峰值，
 * 再使用 ModelInputView::source_crop() 恢复源帧坐标。DMA-BUF 输入为稳定缓冲区
 * 缓存 rknn_set_io_mem() 绑定；CPU 输入每次推理调用 rknn_inputs_set()。
 *
 * @htmlonly
 * <div class="vtb-resource-card">
 *   <div class="vtb-resource-card__title">独占 RKNN 模型资源</div>
 *   <span class="vtb-resource-owned">model_</span> 独占 RknnModel。其内部拥有
 *   rknn_context、可选 DMA-BUF 输入绑定和预分配输出数组；输入绑定销毁时依次执行
 *   rknn_destroy_mem()、munmap()、close(duplicated_fd)，随后模型销毁 context。
 *   析构在实现文件定义，复制和移动均明确删除。
 * </div>
 * <div class="vtb-resource-card vtb-resource-card--borrowed">
 *   <div class="vtb-resource-card__title">同步借用模型输入</div>
 *   detect() 只在调用期间借用 ModelInputView；RknnInputBinding 会复制 DMA-BUF fd，
 *   输入 ModelInputLease 仍需覆盖本次 rknn_run() 和检测后处理。
 * </div>
 * @endhtmlonly
 */
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

/**
 * @brief 在 RKNN Runtime 上执行 66 点模型并生成更新后人脸框与平均置信度。
 *
 * 模型输出契约为一个 198x28x28 张量：通道 0–65 是 66 个热力图，66–131 是
 * 行/Y 偏移，132–197 是列/X 偏移。estimate() 解码每点峰值与偏移，通过
 * ModelInputView::source_crop() 恢复源帧坐标，并计算平均置信度与 updated_box。
 *
 * @htmlonly
 * <div class="vtb-resource-card">
 *   <div class="vtb-resource-card__title">独占 RKNN 模型资源</div>
 *   <span class="vtb-resource-owned">model_</span> 独占另一套 RknnModel context、输入绑定
 *   和输出数组。当前产品装配把检测器放在 Core0、关键点模型放在 Core1；两者分别
 *   管理自己的 RKNN 资源。析构在实现文件定义，复制和移动均明确删除。
 * </div>
 * <div class="vtb-resource-card vtb-resource-card--borrowed">
 *   <div class="vtb-resource-card__title">输出坐标依赖 source_crop</div>
 *   estimate() 在调用期间借用 ModelInputView，并使用其中的 source_crop() 把 66 点从
 *   224×224 模型坐标还原到源帧坐标；调用结束后不保存视图引用。
 * </div>
 * @endhtmlonly
 */
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
