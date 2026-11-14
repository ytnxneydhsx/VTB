#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "vtb/messaging/video/frame_view.hpp"

namespace vtb {

enum class ModelInputLayout {
    nhwc,
    nchw,
};

enum class ModelInputDataType {
    uint8,
    float32,
};

enum class ModelNormalization {
    none,
    external,
    rknn_runtime,
};

class ModelInputSpec {
public:
    ModelInputSpec(
        std::uint32_t width,
        std::uint32_t height,
        VideoPixelFormat pixel_format,
        ModelInputLayout layout,
        ModelInputDataType data_type,
        ModelNormalization normalization,
        std::size_t minimum_buffer_size_bytes = 0);
    ~ModelInputSpec() = default;

    ModelInputSpec(const ModelInputSpec&) = default;
    ModelInputSpec& operator=(const ModelInputSpec&) = default;
    ModelInputSpec(ModelInputSpec&&) noexcept = default;
    ModelInputSpec& operator=(ModelInputSpec&&) noexcept = default;

    std::uint32_t width() const noexcept;
    std::uint32_t height() const noexcept;
    VideoPixelFormat pixel_format() const noexcept;
    ModelInputLayout layout() const noexcept;
    ModelInputDataType data_type() const noexcept;
    ModelNormalization normalization() const noexcept;
    std::size_t minimum_buffer_size_bytes() const noexcept;

private:
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    VideoPixelFormat pixel_format_ = VideoPixelFormat::unknown;
    ModelInputLayout layout_ = ModelInputLayout::nhwc;
    ModelInputDataType data_type_ = ModelInputDataType::uint8;
    ModelNormalization normalization_ = ModelNormalization::none;
    std::size_t minimum_buffer_size_bytes_ = 0;
};

class CropRegion {
public:
    CropRegion(
        std::uint32_t x,
        std::uint32_t y,
        std::uint32_t width,
        std::uint32_t height);
    ~CropRegion() = default;

    CropRegion(const CropRegion&) = default;
    CropRegion& operator=(const CropRegion&) = default;
    CropRegion(CropRegion&&) noexcept = default;
    CropRegion& operator=(CropRegion&&) noexcept = default;

    std::uint32_t x() const noexcept;
    std::uint32_t y() const noexcept;
    std::uint32_t width() const noexcept;
    std::uint32_t height() const noexcept;

private:
    std::uint32_t x_ = 0;
    std::uint32_t y_ = 0;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
};

/**
 * @brief 模型输入帧、输入规格和对应源帧 ROI 的非拥有组合视图。
 *
 * 内部 FrameView 可以复制元数据，但其中的地址和 DMA-BUF fd 仍然借用实际模型输入
 * 缓冲区。调用方必须在对应 ModelInputLease 有效期间使用本对象。
 *
 * @htmlonly
 * <div class="vtb-resource-card vtb-resource-card--borrowed">
 *   <div class="vtb-resource-card__title">借用模型输入存储</div>
 *   <span class="vtb-resource-borrowed">frame_</span> 中的 CPU 地址和 fd 由创建它的
 *   ImageTransformer 及 ModelInputLease 保持有效。ModelInputView 析构时只销毁
 *   按值保存的元数据，实际存储由对应的所有者管理。
 * </div>
 * @endhtmlonly
 */
class ModelInputView {
public:
    ModelInputView(
        FrameView frame,
        ModelInputSpec spec,
        CropRegion source_crop);
    ~ModelInputView() = default;

    ModelInputView(const ModelInputView&) = default;
    ModelInputView& operator=(const ModelInputView&) = default;
    ModelInputView(ModelInputView&&) noexcept = default;
    ModelInputView& operator=(ModelInputView&&) noexcept = default;

    const FrameView& frame() const noexcept;
    const ModelInputSpec& spec() const noexcept;
    const CropRegion& source_crop() const noexcept;

private:
    FrameView frame_;
    ModelInputSpec spec_;
    CropRegion source_crop_;
};

/**
 * @brief 管理一份模型输入视图及其类型擦除保活动作的独占 RAII 租约。
 *
 * 租约不可复制，可以 noexcept 移动；移动会转移保活动作和 valid_ 状态，并把来源
 * 对象重置为可安全析构的无效状态。析构或移动赋值覆盖旧值时执行一次该动作。
 * 当前 RgaImageTransformer 路径把 KeepRgaBufferAlive 保存为该动作；调用体为空，
 * std::function 销毁时释放其中的 shared_ptr，从而延长 RgaBuffer 生命周期。
 *
 * @htmlonly
 * <div class="vtb-resource-card">
 *   <div class="vtb-resource-card__title">独占保活动作</div>
 *   <span class="vtb-resource-owned">return_buffer_ / valid_</span> 共同表示尚未执行的
 *   类型擦除动作；<span class="vtb-resource-owned">view_</span> 按值保存模型输入元数据。
 * </div>
 * <div class="vtb-resource-card vtb-resource-card--borrowed">
 *   <div class="vtb-resource-card__title">view() 的有效期</div>
 *   返回的 <span class="vtb-resource-borrowed">ModelInputView 引用</span> 在当前 Lease
 *   保持有效且地址稳定的期间有效；Lease 析构或移动后该引用随即失效。
 * </div>
 * @endhtmlonly
 */
class ModelInputLease {
public:
    ModelInputLease(ModelInputView view, std::function<void()> return_buffer);
    ~ModelInputLease();

    ModelInputLease(const ModelInputLease&) = delete;
    ModelInputLease& operator=(const ModelInputLease&) = delete;
    ModelInputLease(ModelInputLease&& other) noexcept;
    ModelInputLease& operator=(ModelInputLease&& other) noexcept;

    const ModelInputView& view() const noexcept;

private:
    void reset() noexcept;

    ModelInputView view_;
    std::function<void()> return_buffer_;
    bool valid_ = true;
};

}  // namespace vtb
