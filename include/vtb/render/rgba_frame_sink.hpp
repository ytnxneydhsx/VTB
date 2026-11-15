#pragma once

#include <cstdint>

namespace vtb {

enum class RgbaFrameFormat : std::uint32_t {
    rgba8888 = 1,
};

/**
 * @brief 不拥有资源的 RGBA DMA-BUF 帧描述。
 *
 * RgbaFrameSink 消费者只可在 submit 回调返回前借用内容。LibGodotInstance 返回的
 * 同步视图可借用到下一次 Viewport 帧获取或实例销毁；调用方必须在此之前完成RGA读取。
 * 消费者如需异步持有帧，必须引入显式槽位租约和归还协议。
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

/** @brief LibGodot 向宿主同步交付 RGBA DMA-BUF 的稳定 ABI 回调。 */
class RgbaFrameSink final {
public:
    void* context = nullptr;
    std::int32_t (*submit)(
        void* context, const RgbaFrameView* frame) noexcept = nullptr;
};

}  // namespace vtb
