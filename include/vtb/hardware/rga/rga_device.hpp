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
    rgba8888,
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
 * @brief 由一个业务线程的 RgaTransformer 独占的 librga 同步执行端。
 *
 * RgaBuffer 对象拥有导入 handle、复制 fd 或 DMA heap 分配。缓冲区导入和分配是
 * 与 RGA 核心无关的资源操作，因此由静态成员供 RgaTransformSession 使用；实例
 * 方法只执行已经解析好的同步 RGA 命令。每个实例只由一个 Worker 调用，无需再用
 * 粗粒度互斥锁包围 IM_SYNC。
 */
class RgaDevice final {
public:
    RgaDevice() = default;
    ~RgaDevice() = default;

    RgaDevice(const RgaDevice&) = delete;
    RgaDevice& operator=(const RgaDevice&) = delete;
    RgaDevice(RgaDevice&&) = delete;
    RgaDevice& operator=(RgaDevice&&) = delete;

    static std::shared_ptr<RgaBuffer> register_dma_buf(
        const RgaBufferDescription& description);
    static std::shared_ptr<RgaBuffer> allocate_buffer(
        const std::string& dma_heap_path,
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
    void convert_rgba_to_nv12(
        const RgaBuffer& source,
        const RgaBuffer& destination);
    void convert_rgba_to_nv12_flipped_vertically(
        const RgaBuffer& source,
        const RgaBuffer& destination);
};

}  // namespace vtb
