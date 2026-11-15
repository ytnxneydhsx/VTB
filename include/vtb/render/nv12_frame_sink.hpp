#pragma once

#include <cstdint>
#include <memory>
#include <utility>

namespace vtb {

/** @brief NV12 DMA-BUF 帧描述。 */
class Nv12FrameView final {
public:
    int dma_buf_fd = -1;
    std::uint64_t dma_buf_size = 0;
    std::uint64_t buffer_id = 0;
    std::uint64_t stream_generation = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t horizontal_stride = 0;
    std::uint32_t vertical_stride = 0;
    std::uint32_t frames_per_second = 0;
    std::uint64_t timestamp_ns = 0;
    std::uint64_t sequence = 0;
    std::int32_t end_of_stream = 0;
};

/**
 * @brief 独占一张 NV12 帧的目标池槽位寿命。
 *
 * view() 只在本 Lease 有效期间借用 DMA-BUF。keep_alive 保存类型擦除后的
 * RgaTransformLease；Lease 析构后目标槽位才允许被后续转换复用。
 */
class Nv12FrameLease final {
public:
    Nv12FrameLease(
        Nv12FrameView view,
        std::shared_ptr<void> keep_alive)
        : view_(view), keep_alive_(std::move(keep_alive)) {}
    ~Nv12FrameLease() = default;

    Nv12FrameLease(const Nv12FrameLease&) = delete;
    Nv12FrameLease& operator=(const Nv12FrameLease&) = delete;
    Nv12FrameLease(Nv12FrameLease&&) noexcept = default;
    Nv12FrameLease& operator=(Nv12FrameLease&&) noexcept = default;

    const Nv12FrameView& view() const noexcept { return view_; }

private:
    Nv12FrameView view_;
    std::shared_ptr<void> keep_alive_;
};

/**
 * @brief 把 NV12 DMA-BUF 槽位租约移动给异步媒体消费者的宿主回调。
 *
 * submit 成功后消费者必须从 frame 移走租约；最后一个异步消费者释放租约时，
 * RGA 目标槽位才重新可用。失败时调用方仍拥有 frame，可按正常析构路径归还。
 */
class Nv12FrameSink final {
public:
    void* context = nullptr;
    std::int32_t (*submit)(
        void* context, Nv12FrameLease* frame) noexcept = nullptr;
};

}  // namespace vtb
