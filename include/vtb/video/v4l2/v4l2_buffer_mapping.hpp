#pragma once

#include <cstddef>

namespace vtb {

/**
 * @brief 独占一块 V4L2 Capture Buffer 的 MMAP 映射和可选 DMA-BUF fd。
 * @ingroup video_input_resources
 *
 * 地址和 fd 都是借用访问入口；只有当前对象负责 munmap() 和 close()。类支持
 * vector 当前所需的移动构造，不支持复制和移动赋值；移动构造后来源对象恢复为空状态。
 */
class V4l2BufferMapping final {
public:
    V4l2BufferMapping() noexcept;
    V4l2BufferMapping(
        void* address, std::size_t length, int dma_buf_fd) noexcept;
    ~V4l2BufferMapping();

    V4l2BufferMapping(const V4l2BufferMapping&) = delete;
    V4l2BufferMapping& operator=(const V4l2BufferMapping&) = delete;
    V4l2BufferMapping(V4l2BufferMapping&& other) noexcept;
    V4l2BufferMapping& operator=(V4l2BufferMapping&& other) = delete;

    const std::byte* data() const noexcept;
    std::size_t length() const noexcept;
    int dma_buf_fd() const noexcept;

private:
    void reset() noexcept;

    void* address_;
    std::size_t length_ = 0;
    int dma_buf_fd_ = -1;
};

}  // namespace vtb
