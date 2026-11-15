#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "vtb/hardware/rga/rga_executor.hpp"

namespace vtb {

/** @brief 业务帧转换为 RGA 源缓存键与导入描述后的值对象。 */
class RgaSourceFrame final {
public:
    RgaSourceFrame(
        int dma_buf_fd,
        std::size_t size_bytes,
        std::uint64_t buffer_id,
        std::uint64_t stream_generation,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t horizontal_stride,
        std::uint32_t vertical_stride,
        std::uint32_t stride_bytes,
        RgaPixelFormat format);
    ~RgaSourceFrame() = default;

    RgaSourceFrame(const RgaSourceFrame&) = default;
    RgaSourceFrame& operator=(const RgaSourceFrame&) = default;
    RgaSourceFrame(RgaSourceFrame&&) noexcept = default;
    RgaSourceFrame& operator=(RgaSourceFrame&&) noexcept = default;

    int dma_buf_fd() const noexcept;
    std::size_t size_bytes() const noexcept;
    std::uint64_t buffer_id() const noexcept;
    std::uint64_t stream_generation() const noexcept;
    std::uint32_t width() const noexcept;
    std::uint32_t height() const noexcept;
    std::uint32_t horizontal_stride() const noexcept;
    std::uint32_t vertical_stride() const noexcept;
    std::uint32_t stride_bytes() const noexcept;
    RgaPixelFormat format() const noexcept;

private:
    int dma_buf_fd_ = -1;
    std::size_t size_bytes_ = 0;
    std::uint64_t buffer_id_ = 0;
    std::uint64_t stream_generation_ = 0;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t horizontal_stride_ = 0;
    std::uint32_t vertical_stride_ = 0;
    std::uint32_t stride_bytes_ = 0;
    RgaPixelFormat format_ = RgaPixelFormat::rgb888;
};

/** @brief 一次通用转换的源区域和目标 DMA-BUF 规格。 */
class RgaTransformSpec final {
public:
    RgaTransformSpec(
        RgaTransformOperation operation,
        RgaRectangle source_rectangle,
        std::uint32_t destination_width,
        std::uint32_t destination_height,
        std::uint32_t destination_horizontal_stride,
        std::uint32_t destination_vertical_stride,
        std::uint32_t destination_stride_bytes,
        std::size_t destination_minimum_size_bytes,
        RgaPixelFormat destination_format);
    ~RgaTransformSpec() = default;

    RgaTransformSpec(const RgaTransformSpec&) = default;
    RgaTransformSpec& operator=(const RgaTransformSpec&) = default;
    RgaTransformSpec(RgaTransformSpec&&) noexcept = default;
    RgaTransformSpec& operator=(RgaTransformSpec&&) noexcept = default;

    RgaTransformOperation operation() const noexcept;
    const RgaRectangle& source_rectangle() const noexcept;
    std::uint32_t destination_width() const noexcept;
    std::uint32_t destination_height() const noexcept;
    std::uint32_t destination_horizontal_stride() const noexcept;
    std::uint32_t destination_vertical_stride() const noexcept;
    std::uint32_t destination_stride_bytes() const noexcept;
    std::size_t destination_minimum_size_bytes() const noexcept;
    RgaPixelFormat destination_format() const noexcept;

private:
    RgaTransformOperation operation_ =
        RgaTransformOperation::crop_scale_convert;
    RgaRectangle source_rectangle_{0, 0, 0, 0};
    std::uint32_t destination_width_ = 0;
    std::uint32_t destination_height_ = 0;
    std::uint32_t destination_horizontal_stride_ = 0;
    std::uint32_t destination_vertical_stride_ = 0;
    std::uint32_t destination_stride_bytes_ = 0;
    std::size_t destination_minimum_size_bytes_ = 0;
    RgaPixelFormat destination_format_ = RgaPixelFormat::rgb888;
};

/** @brief 独占一个目标池槽位并保持其 RgaBuffer 有效的移动租约。 */
class RgaTransformLease final {
public:
    ~RgaTransformLease() = default;

    RgaTransformLease(const RgaTransformLease&) = delete;
    RgaTransformLease& operator=(const RgaTransformLease&) = delete;
    RgaTransformLease(RgaTransformLease&&) noexcept = default;
    RgaTransformLease& operator=(RgaTransformLease&&) noexcept = default;

    const std::shared_ptr<RgaBuffer>& buffer() const noexcept;
    std::uint64_t buffer_id() const noexcept;

private:
    RgaTransformLease(
        std::shared_ptr<RgaBuffer> buffer,
        std::uint64_t buffer_id,
        std::shared_ptr<void> slot_token);

    std::shared_ptr<RgaBuffer> buffer_;
    std::uint64_t buffer_id_ = 0;
    std::shared_ptr<void> slot_token_;
    friend class RgaTransformSession;
};

/**
 * @brief 一个业务生产者独占的 RGA 导入缓存、目标池和同步提交会话。
 *
 * Session 每次 transform() 只允许一个调用进入，但不同 Session 可同时向共享
 * RgaExecutor 提交。源注册按 fd、buffer_id、stream_generation、尺寸、stride 和
 * 格式缓存；目标池槽位由 RgaTransformLease 的寿命控制。
 */
class RgaTransformSession final {
public:
    explicit RgaTransformSession(
        std::shared_ptr<RgaExecutor> executor,
        std::size_t destination_pool_capacity = 2);
    ~RgaTransformSession();

    RgaTransformSession(const RgaTransformSession&) = delete;
    RgaTransformSession& operator=(const RgaTransformSession&) = delete;
    RgaTransformSession(RgaTransformSession&&) = delete;
    RgaTransformSession& operator=(RgaTransformSession&&) = delete;

    void begin_source(std::uint64_t stream_generation);
    RgaTransformLease transform(
        const RgaSourceFrame& source,
        const RgaTransformSpec& spec);
    void retire_source(std::uint64_t stream_generation) noexcept;
    void finish() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vtb
