#include "vtb/video/v4l2/v4l2_buffer_mapping.hpp"

#include <sys/mman.h>
#include <unistd.h>

namespace vtb {

V4l2BufferMapping::V4l2BufferMapping() noexcept
    : address_(MAP_FAILED) {}

V4l2BufferMapping::V4l2BufferMapping(
    void* address,
    std::size_t length,
    int dma_buf_fd) noexcept
    : address_(address), length_(length), dma_buf_fd_(dma_buf_fd) {}

V4l2BufferMapping::~V4l2BufferMapping() {
    reset();
}

V4l2BufferMapping::V4l2BufferMapping(
    V4l2BufferMapping&& other) noexcept
    : address_(other.address_),
      length_(other.length_),
      dma_buf_fd_(other.dma_buf_fd_) {
    other.address_ = MAP_FAILED;
    other.length_ = 0;
    other.dma_buf_fd_ = -1;
}

const std::byte* V4l2BufferMapping::data() const noexcept {
    return static_cast<const std::byte*>(address_);
}

std::size_t V4l2BufferMapping::length() const noexcept {
    return length_;
}

int V4l2BufferMapping::dma_buf_fd() const noexcept {
    return dma_buf_fd_;
}

void V4l2BufferMapping::reset() noexcept {
    if (address_ != MAP_FAILED) {
        munmap(address_, length_);
    }
    if (dma_buf_fd_ >= 0) {
        close(dma_buf_fd_);
    }
    address_ = MAP_FAILED;
    length_ = 0;
    dma_buf_fd_ = -1;
}

}  // namespace vtb
