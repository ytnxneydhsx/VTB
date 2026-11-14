#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace vtb {

enum class RgaPixelFormat {
    yuyv422,
    nv12,
    nv21,
    rgb888,
};

class RgaBufferDescription final {
public:
    RgaBufferDescription(
        int dma_buf_fd,
        std::size_t size_bytes,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t horizontal_stride,
        std::uint32_t vertical_stride,
        std::uint32_t stride_bytes,
        RgaPixelFormat format);
    ~RgaBufferDescription() = default;

    RgaBufferDescription(const RgaBufferDescription&) = default;
    RgaBufferDescription& operator=(const RgaBufferDescription&) = default;
    RgaBufferDescription(RgaBufferDescription&&) noexcept = default;
    RgaBufferDescription& operator=(RgaBufferDescription&&) noexcept = default;

    int dma_buf_fd() const noexcept;
    std::size_t size_bytes() const noexcept;
    std::uint32_t width() const noexcept;
    std::uint32_t height() const noexcept;
    std::uint32_t horizontal_stride() const noexcept;
    std::uint32_t vertical_stride() const noexcept;
    std::uint32_t stride_bytes() const noexcept;
    RgaPixelFormat format() const noexcept;

private:
    int dma_buf_fd_ = -1;
    std::size_t size_bytes_ = 0;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t horizontal_stride_ = 0;
    std::uint32_t vertical_stride_ = 0;
    std::uint32_t stride_bytes_ = 0;
    RgaPixelFormat format_ = RgaPixelFormat::rgb888;
};

class RgaRectangle final {
public:
    RgaRectangle(
        std::uint32_t x,
        std::uint32_t y,
        std::uint32_t width,
        std::uint32_t height) noexcept;
    ~RgaRectangle() = default;

    RgaRectangle(const RgaRectangle&) = default;
    RgaRectangle& operator=(const RgaRectangle&) = default;
    RgaRectangle(RgaRectangle&&) noexcept = default;
    RgaRectangle& operator=(RgaRectangle&&) noexcept = default;

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

/** @brief 一个已导入 RGA 的 DMA-BUF 注册租约。 */
class RgaBuffer final {
public:
    ~RgaBuffer();

    RgaBuffer(const RgaBuffer&) = delete;
    RgaBuffer& operator=(const RgaBuffer&) = delete;
    RgaBuffer(RgaBuffer&&) = delete;
    RgaBuffer& operator=(RgaBuffer&&) = delete;

    const std::byte* data() const noexcept;
    int dma_buf_fd() const noexcept;
    std::size_t size_bytes() const noexcept;
    std::uint32_t width() const noexcept;
    std::uint32_t height() const noexcept;
    std::uint32_t stride_bytes() const noexcept;
    RgaPixelFormat format() const noexcept;

private:
    class Impl;
    explicit RgaBuffer(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
    friend class RgaDevice;
};

/**
 * @brief 可由同一业务模块内多个 RGA 适配器共享的执行资源。
 *
 * Buffer 对象分别拥有导入 handle、复制 fd 或 DMA heap 分配。设备对象保存 DMA heap
 * 配置并串行保护 librga 提交入口；它不创建工作线程。
 */
class RgaDevice final {
public:
    explicit RgaDevice(std::string dma_heap_path);
    ~RgaDevice();

    RgaDevice(const RgaDevice&) = delete;
    RgaDevice& operator=(const RgaDevice&) = delete;
    RgaDevice(RgaDevice&&) = delete;
    RgaDevice& operator=(RgaDevice&&) = delete;

    std::shared_ptr<RgaBuffer> register_dma_buf(
        const RgaBufferDescription& description);
    std::shared_ptr<RgaBuffer> allocate_buffer(
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t horizontal_stride,
        std::uint32_t vertical_stride,
        std::uint32_t stride_bytes,
        std::size_t minimum_size_bytes,
        RgaPixelFormat format);
    void crop_scale_convert(
        const RgaBuffer& source,
        const RgaRectangle& source_rectangle,
        const RgaBuffer& destination);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vtb
