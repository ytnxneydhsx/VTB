#pragma once

#include <cstdint>
#include <memory>
#include <utility>

namespace vtb {

enum class RgbaFrameFormat : std::uint32_t {
    rgba8888 = 1,
};

/**
 * @brief 不拥有资源的 RGBA DMA-BUF 帧描述。
 *
 * 视图中的 fd 在对应的 RgbaFrameLease 持有期间可用。
 */
class RgbaFrameView final {
public:
    int dma_buf_fd = -1;
    std::uint64_t dma_buf_size = 0;
    std::uint64_t buffer_id = 0;
    std::uint64_t stream_generation = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t horizontal_stride = 0;
    std::uint32_t vertical_stride = 0;
    std::uint32_t stride_bytes = 0;
    std::uint32_t frames_per_second = 0;
    std::uint64_t timestamp_ns = 0;
    std::uint64_t sequence = 0;
    RgbaFrameFormat format = RgbaFrameFormat::rgba8888;
    std::int32_t end_of_stream = 0;
};

/**
 * @brief 独占一次 RGBA 帧交付，析构时归还 GPU 渲染槽位。
 *
 * 槽位的持有对象保活 DMA-BUF。消费者将租约绑定到 GstMemory 后，
 * 最后一个内存引用释放时才归还槽位。
 */
class RgbaFrameLease final {
public:
    RgbaFrameLease(RgbaFrameView view, std::shared_ptr<void> slot) noexcept
        : view_(view), slot_(std::move(slot)) {}
    ~RgbaFrameLease() = default;

    RgbaFrameLease(const RgbaFrameLease&) = delete;
    RgbaFrameLease& operator=(const RgbaFrameLease&) = delete;
    RgbaFrameLease(RgbaFrameLease&&) noexcept = default;
    RgbaFrameLease& operator=(RgbaFrameLease&&) noexcept = delete;

    const RgbaFrameView& view() const noexcept { return view_; }

private:
    RgbaFrameView view_;
    std::shared_ptr<void> slot_;
};

/** @brief LibGodot 通过回调把 RGBA 租约移动交付给宿主。 */
class RgbaFrameSink final {
public:
    void* context = nullptr;
    std::int32_t (*submit)(
        void* context, RgbaFrameLease* frame) noexcept = nullptr;
};

}  // namespace vtb
