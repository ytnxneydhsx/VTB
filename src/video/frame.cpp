#include "vtb/video/frame.hpp"

#include <stdexcept>
#include <utility>

namespace vtb {

FramePlaneView::FramePlaneView(
    const std::byte* data,
    std::size_t size_bytes,
    std::uint32_t stride_bytes,
    int dma_buf_fd,
    std::uint32_t offset_bytes)
    : data_(data),
      size_bytes_(size_bytes),
      stride_bytes_(stride_bytes),
      dma_buf_fd_(dma_buf_fd),
      offset_bytes_(offset_bytes) {
    if (size_bytes_ == 0 || stride_bytes_ == 0) {
        throw std::invalid_argument("Frame plane size and stride must be non-zero");
    }
    if (data_ == nullptr && dma_buf_fd_ < 0) {
        throw std::invalid_argument(
            "Frame plane requires CPU data or a DMA-BUF descriptor");
    }
}

const std::byte* FramePlaneView::data() const noexcept {
    return data_;
}

std::size_t FramePlaneView::size_bytes() const noexcept {
    return size_bytes_;
}

std::uint32_t FramePlaneView::stride_bytes() const noexcept {
    return stride_bytes_;
}

int FramePlaneView::dma_buf_fd() const noexcept {
    return dma_buf_fd_;
}

std::uint32_t FramePlaneView::offset_bytes() const noexcept {
    return offset_bytes_;
}

FrameView::FrameView(
    VideoPixelFormat pixel_format,
    FrameMemoryType memory_type,
    std::uint32_t width,
    std::uint32_t height,
    std::uint64_t frame_id,
    std::uint64_t timestamp_ns,
    FrameTimestampSource timestamp_source,
    std::array<FramePlaneView, max_planes> planes,
    std::size_t plane_count)
    : pixel_format_(pixel_format),
      memory_type_(memory_type),
      width_(width),
      height_(height),
      frame_id_(frame_id),
      timestamp_ns_(timestamp_ns),
      timestamp_source_(timestamp_source),
      planes_(std::move(planes)),
      plane_count_(plane_count) {
    if (width_ == 0 || height_ == 0) {
        throw std::invalid_argument("Frame dimensions must be non-zero");
    }
    if (pixel_format_ == VideoPixelFormat::unknown) {
        throw std::invalid_argument("Frame pixel format must be known");
    }
    if (plane_count_ == 0 || plane_count_ > max_planes) {
        throw std::invalid_argument("FrameView plane count is out of range");
    }
    for (std::size_t index = 0; index < plane_count_; ++index) {
        if (memory_type_ == FrameMemoryType::cpu &&
            planes_[index].data() == nullptr) {
            throw std::invalid_argument("CPU frame plane requires an address");
        }
        if (memory_type_ == FrameMemoryType::dma_buf &&
            planes_[index].dma_buf_fd() < 0) {
            throw std::invalid_argument(
                "DMA-BUF frame plane requires a file descriptor");
        }
    }
}

VideoPixelFormat FrameView::pixel_format() const noexcept {
    return pixel_format_;
}

FrameMemoryType FrameView::memory_type() const noexcept {
    return memory_type_;
}

std::uint32_t FrameView::width() const noexcept {
    return width_;
}

std::uint32_t FrameView::height() const noexcept {
    return height_;
}

std::uint64_t FrameView::frame_id() const noexcept {
    return frame_id_;
}

std::uint64_t FrameView::timestamp_ns() const noexcept {
    return timestamp_ns_;
}

FrameTimestampSource FrameView::timestamp_source() const noexcept {
    return timestamp_source_;
}

std::size_t FrameView::plane_count() const noexcept {
    return plane_count_;
}

const FramePlaneView& FrameView::plane(std::size_t index) const {
    if (index >= plane_count_) {
        throw std::out_of_range("FrameView plane index is out of range");
    }
    return planes_[index];
}

DecodedFrameLease::DecodedFrameLease(
    FrameView view,
    std::function<void()> return_buffer)
    : view_(std::move(view)),
      return_buffer_(std::move(return_buffer)),
      valid_(true) {}

DecodedFrameLease::~DecodedFrameLease() {
    reset();
}

DecodedFrameLease::DecodedFrameLease(DecodedFrameLease&& other) noexcept
    : view_(std::move(other.view_)),
      return_buffer_(std::move(other.return_buffer_)),
      valid_(other.valid_) {
    other.valid_ = false;
    other.return_buffer_ = {};
}

DecodedFrameLease& DecodedFrameLease::operator=(
    DecodedFrameLease&& other) noexcept {
    if (this != &other) {
        reset();
        view_ = std::move(other.view_);
        return_buffer_ = std::move(other.return_buffer_);
        valid_ = other.valid_;
        other.valid_ = false;
        other.return_buffer_ = {};
    }
    return *this;
}

const FrameView& DecodedFrameLease::view() const noexcept {
    return view_;
}

DecodedFrameLease::operator bool() const noexcept {
    return valid_;
}

void DecodedFrameLease::reset() noexcept {
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
