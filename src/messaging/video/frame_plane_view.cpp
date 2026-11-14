#include "vtb/messaging/video/frame_plane_view.hpp"

#include <stdexcept>

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

}  // namespace vtb
