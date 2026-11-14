#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>

#include "vtb/messaging/video/decoded_frame_lease.hpp"

namespace vtb {

/**
 * @brief 在线程之间保存至多一帧最新租约的同步槽位。
 *
 * publish() 把新租约移动到 frame_。槽位中原有租约被替换后在锁外析构，从而触发
 * QBUF；wait_and_take() 将最新租约移动给消费者。槽位不复制帧租约。
 *
 * @htmlonly
 * <div class="vtb-resource-card">
 *   <div class="vtb-resource-card__title">临时帧所有权与同步资源</div>
 *   <span class="vtb-resource-owned">frame_</span> 临时拥有最新帧的归还责任；
 *   <span class="vtb-resource-owned">mutex_ / condition_</span> 管理生产者与消费者同步。
 *   这些成员使对象保持固定地址，并要求复制和移动全部删除。
 * </div>
 * @endhtmlonly
 */
class LatestFrameSlot {
public:
    LatestFrameSlot() = default;
    ~LatestFrameSlot() = default;

    /** @brief 禁止复制帧租约和同步资源。 */
    LatestFrameSlot(const LatestFrameSlot&) = delete;
    /** @brief 禁止复制赋值帧租约和同步资源。 */
    LatestFrameSlot& operator=(const LatestFrameSlot&) = delete;
    /** @brief mutex、condition_variable 和等待关系要求对象地址稳定。 */
    LatestFrameSlot(LatestFrameSlot&&) = delete;
    /** @brief 禁止通过移动赋值改变同步槽位地址。 */
    LatestFrameSlot& operator=(LatestFrameSlot&&) = delete;

    bool publish(DecodedFrameLease frame);
    std::optional<DecodedFrameLease> wait_and_take();
    void finish() noexcept;
    void close() noexcept;

    std::uint64_t published_count() const noexcept;
    std::uint64_t replaced_count() const noexcept;

private:
    /** @brief <span class="vtb-resource-owned">同步资源</span> 保护槽位全部共享状态。 */
    mutable std::mutex mutex_;
    /** @brief <span class="vtb-resource-owned">同步资源</span> 唤醒等待最新帧的消费者。 */
    std::condition_variable condition_;
    /** @brief <span class="vtb-resource-owned">临时所有权</span> 最新且尚未取走的帧租约。 */
    std::optional<DecodedFrameLease> frame_;
    bool finished_ = false;
    std::uint64_t published_count_ = 0;
    std::uint64_t replaced_count_ = 0;
};

}  // namespace vtb
