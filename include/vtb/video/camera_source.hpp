#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "vtb/capture_config.hpp"
#include "vtb/video/video_source.hpp"

namespace vtb {

class V4l2BufferPool;

/**
 * @brief 面向 VideoSource 接口的 V4L2 摄像头采集入口。
 *
 * CameraSource 共享持有 V4l2BufferPool。read() 从缓冲池取得一帧租约；close()
 * 释放本对象持有的 shared_ptr。已经交给下游的租约仍可继续持有缓冲池，直到最后
 * 一帧归还后才释放 MMAP、DMA-BUF fd 和摄像头 fd。
 *
 * @htmlonly
 * <div class="vtb-resource-card">
 *   <div class="vtb-resource-card__title">共享资源所有权</div>
 *   <span class="vtb-resource-owned">buffer_pool_</span> 是缓冲池的共享所有权句柄。
 *   close() 释放当前句柄；尚未析构的 DecodedFrameLease 可以继续延长缓冲池生命期。
 * </div>
 * @endhtmlonly
 */
class CameraSource final : public VideoSource {
public:
    explicit CameraSource(CaptureConfig config, std::uint32_t buffer_count = 4);
    ~CameraSource() override;

    /** @brief 禁止复制共享采集入口。 */
    CameraSource(const CameraSource&) = delete;
    /** @brief 禁止复制赋值共享采集入口。 */
    CameraSource& operator=(const CameraSource&) = delete;
    /** @brief 缓冲池和租约回调依赖稳定的对象关系，当前禁止移动。 */
    CameraSource(CameraSource&&) = delete;
    /** @brief 当前禁止通过移动赋值改变采集入口所有权。 */
    CameraSource& operator=(CameraSource&&) = delete;

    std::optional<DecodedFrameLease> read() override;
    void request_stop() noexcept override;
    void close() noexcept override;

private:
    /** @brief <span class="vtb-resource-owned">共享所有权</span> V4L2 采集缓冲池。 */
    std::shared_ptr<V4l2BufferPool> buffer_pool_;
};

}  // namespace vtb
