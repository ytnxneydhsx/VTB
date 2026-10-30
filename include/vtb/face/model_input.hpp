#pragma once

#include <cstdint>
#include <functional>

#include "vtb/video/frame.hpp"

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
        ModelNormalization normalization);
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

private:
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    VideoPixelFormat pixel_format_ = VideoPixelFormat::unknown;
    ModelInputLayout layout_ = ModelInputLayout::nhwc;
    ModelInputDataType data_type_ = ModelInputDataType::uint8;
    ModelNormalization normalization_ = ModelNormalization::none;
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
