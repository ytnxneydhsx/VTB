#include "vtb/face/model_input.hpp"

#include <stdexcept>
#include <utility>

namespace vtb {

ModelInputSpec::ModelInputSpec(
    std::uint32_t width,
    std::uint32_t height,
    VideoPixelFormat pixel_format,
    ModelInputLayout layout,
    ModelInputDataType data_type,
    ModelNormalization normalization,
    std::size_t minimum_buffer_size_bytes)
    : width_(width),
      height_(height),
      pixel_format_(pixel_format),
      layout_(layout),
      data_type_(data_type),
      normalization_(normalization),
      minimum_buffer_size_bytes_(minimum_buffer_size_bytes) {
    if (width_ == 0 || height_ == 0) {
        throw std::invalid_argument("Model input dimensions must be non-zero");
    }
    if (pixel_format_ == VideoPixelFormat::unknown) {
        throw std::invalid_argument("Model input pixel format must be known");
    }
    const std::size_t packed_rgb_size =
        static_cast<std::size_t>(width_) *
        static_cast<std::size_t>(height_) * 3U;
    if (minimum_buffer_size_bytes_ == 0) {
        minimum_buffer_size_bytes_ = packed_rgb_size;
    }
    if (pixel_format_ == VideoPixelFormat::rgb888 &&
        minimum_buffer_size_bytes_ < packed_rgb_size) {
        throw std::invalid_argument(
            "Model input buffer is smaller than packed RGB888 data");
    }
}

std::uint32_t ModelInputSpec::width() const noexcept {
    return width_;
}

std::uint32_t ModelInputSpec::height() const noexcept {
    return height_;
}

VideoPixelFormat ModelInputSpec::pixel_format() const noexcept {
    return pixel_format_;
}

ModelInputLayout ModelInputSpec::layout() const noexcept {
    return layout_;
}

ModelInputDataType ModelInputSpec::data_type() const noexcept {
    return data_type_;
}

ModelNormalization ModelInputSpec::normalization() const noexcept {
    return normalization_;
}

std::size_t ModelInputSpec::minimum_buffer_size_bytes() const noexcept {
    return minimum_buffer_size_bytes_;
}

CropRegion::CropRegion(
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t width,
    std::uint32_t height)
    : x_(x), y_(y), width_(width), height_(height) {
    if (width_ == 0 || height_ == 0) {
        throw std::invalid_argument("Crop dimensions must be non-zero");
    }
}

std::uint32_t CropRegion::x() const noexcept {
    return x_;
}

std::uint32_t CropRegion::y() const noexcept {
    return y_;
}

std::uint32_t CropRegion::width() const noexcept {
    return width_;
}

std::uint32_t CropRegion::height() const noexcept {
    return height_;
}

ModelInputView::ModelInputView(
    FrameView frame,
    ModelInputSpec spec,
    CropRegion source_crop)
    : frame_(std::move(frame)),
      spec_(std::move(spec)),
      source_crop_(std::move(source_crop)) {}

const FrameView& ModelInputView::frame() const noexcept {
    return frame_;
}

const ModelInputSpec& ModelInputView::spec() const noexcept {
    return spec_;
}

const CropRegion& ModelInputView::source_crop() const noexcept {
    return source_crop_;
}

ModelInputLease::ModelInputLease(
    ModelInputView view,
    std::function<void()> return_buffer)
    : view_(std::move(view)), return_buffer_(std::move(return_buffer)) {}

ModelInputLease::~ModelInputLease() {
    reset();
}

ModelInputLease::ModelInputLease(ModelInputLease&& other) noexcept
    : view_(std::move(other.view_)),
      return_buffer_(std::move(other.return_buffer_)),
      valid_(other.valid_) {
    other.return_buffer_ = {};
    other.valid_ = false;
}

ModelInputLease& ModelInputLease::operator=(ModelInputLease&& other) noexcept {
    if (this != &other) {
        reset();
        view_ = std::move(other.view_);
        return_buffer_ = std::move(other.return_buffer_);
        valid_ = other.valid_;
        other.return_buffer_ = {};
        other.valid_ = false;
    }
    return *this;
}

const ModelInputView& ModelInputLease::view() const noexcept {
    return view_;
}

void ModelInputLease::reset() noexcept {
    if (valid_ && return_buffer_) {
        try {
            return_buffer_();
        } catch (...) {
        }
    }
    return_buffer_ = {};
    valid_ = false;
}

}  // namespace vtb
