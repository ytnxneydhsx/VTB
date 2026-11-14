#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "vtb/capture_config.hpp"
#include "vtb/messaging/video/decoded_frame_lease.hpp"
#include "vtb/v4l2_camera.hpp"
#include "vtb/video/v4l2/v4l2_buffer_mapping.hpp"

namespace vtb {

/**
 * @brief 管理 V4L2 MMAP Capture Buffer 及 DQBUF 到 QBUF 的完整生命周期。
 * @ingroup video_input_buffer_pool
 *
 * 缓冲池由 CameraSource 和尚未释放的帧租约通过 shared_ptr 共同保活。类自身以
 * 值成员独占 V4l2Camera，间接拥有唯一设备 fd；同时拥有全部
 * V4l2BufferMapping 和回队同步状态，不创建线程。
 */
class V4l2BufferPool final
    : public std::enable_shared_from_this<V4l2BufferPool> {
private:
    /** @brief 租约析构时保持缓冲池存活，并把指定槽位重新 QBUF。 */
    class RequeueOnLeaseRelease final {
    public:
        RequeueOnLeaseRelease(
            std::shared_ptr<V4l2BufferPool> owner,
            std::uint32_t index) noexcept;
        ~RequeueOnLeaseRelease() = default;

        RequeueOnLeaseRelease(const RequeueOnLeaseRelease&) = default;
        RequeueOnLeaseRelease& operator=(
            const RequeueOnLeaseRelease&) = default;
        RequeueOnLeaseRelease(RequeueOnLeaseRelease&&) noexcept = default;
        RequeueOnLeaseRelease& operator=(
            RequeueOnLeaseRelease&&) noexcept = default;

        void operator()() const noexcept;

    private:
        std::shared_ptr<V4l2BufferPool> owner_;
        std::uint32_t index_ = 0;
    };

public:
    /**
     * @brief 原地构造摄像头、应用配置并初始化 MMAP 缓冲池。
     * @param requested 摄像头设备、格式、分辨率和帧率配置。
     * @param requested_buffer_count 申请的 Capture Buffer 数量。
     *
     * requested_ 保留帧描述所需的配置；camera_ 使用该配置打开设备并
     * 在缓冲池整个生命期内独占设备 fd。
     */
    V4l2BufferPool(
        CaptureConfig requested,
        std::uint32_t requested_buffer_count);
    ~V4l2BufferPool();

    V4l2BufferPool(const V4l2BufferPool&) = delete;
    V4l2BufferPool& operator=(const V4l2BufferPool&) = delete;
    V4l2BufferPool(V4l2BufferPool&&) = delete;
    V4l2BufferPool& operator=(V4l2BufferPool&&) = delete;

    std::optional<DecodedFrameLease> dequeue();
    void request_stop() noexcept;

private:
    void initialize(std::uint32_t requested_buffer_count);
    void requeue(std::uint32_t index) noexcept;
    void shutdown() noexcept;

    /** @brief 帧元数据构造使用的采集配置。 */
    CaptureConfig requested_;
    /** @brief 值成员独占摄像头设备 fd，在 Mapping 释放后析构。 */
    V4l2Camera camera_;
    std::uint32_t bytes_per_line_ = 0;
    std::vector<V4l2BufferMapping> mappings_;
    std::mutex requeue_mutex_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<int> requeue_error_{0};
    bool streaming_ = false;
};

}  // namespace vtb
