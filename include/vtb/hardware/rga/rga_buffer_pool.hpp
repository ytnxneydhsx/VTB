#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace vtb {

class RgaBuffer;
class RgaBufferPool;
class RgaTransformSpec;

/** @brief 独占一个 RgaBufferPool 槽位并保持其 RgaBuffer 有效的移动租约。 */
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
        std::shared_ptr<void> return_token);

    std::shared_ptr<RgaBuffer> buffer_;
    std::uint64_t buffer_id_ = 0;
    std::shared_ptr<void> return_token_;
    friend class RgaBufferPool;
};

/**
 * @brief 管理可复用 RGA 输出 DMA-BUF 及其槽位租约的独立缓冲池。
 *
 * 缓冲池按首个转换规格创建固定数量的 RgaBuffer。acquire() 返回的
 * RgaTransformLease 保持池和目标槽位有效；最后一份 Lease 释放后，槽位自动回池。
 * 规格变化时，缓冲池等待全部旧槽位归还，再按新规格整体重建。
 */
class RgaBufferPool final
    : public std::enable_shared_from_this<RgaBufferPool> {
private:
    /** @brief Lease 最后释放时保持缓冲池存活，并归还指定槽位。 */
    class ReleaseOnLeaseDestroy final {
    public:
        ReleaseOnLeaseDestroy(
            std::shared_ptr<RgaBufferPool> owner,
            std::size_t index) noexcept;
        ~ReleaseOnLeaseDestroy();

        ReleaseOnLeaseDestroy(const ReleaseOnLeaseDestroy&) = delete;
        ReleaseOnLeaseDestroy& operator=(
            const ReleaseOnLeaseDestroy&) = delete;
        ReleaseOnLeaseDestroy(ReleaseOnLeaseDestroy&&) = delete;
        ReleaseOnLeaseDestroy& operator=(ReleaseOnLeaseDestroy&&) = delete;

    private:
        std::shared_ptr<RgaBufferPool> owner_;
        std::size_t index_ = 0;
    };

public:
    RgaBufferPool(std::string dma_heap_path, std::size_t capacity);
    ~RgaBufferPool();

    RgaBufferPool(const RgaBufferPool&) = delete;
    RgaBufferPool& operator=(const RgaBufferPool&) = delete;
    RgaBufferPool(RgaBufferPool&&) = delete;
    RgaBufferPool& operator=(RgaBufferPool&&) = delete;

    RgaTransformLease acquire(const RgaTransformSpec& transform_spec);

private:
    void release(std::size_t index) noexcept;

    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vtb
