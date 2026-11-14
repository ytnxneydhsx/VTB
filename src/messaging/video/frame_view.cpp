#include "vtb/messaging/video/frame_view.hpp"

#include <stdexcept>
#include <utility>

namespace vtb {

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

}  // namespace vtb
