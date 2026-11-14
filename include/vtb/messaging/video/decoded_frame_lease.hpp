#pragma once

#include <functional>

#include "vtb/messaging/video/frame_view.hpp"

namespace vtb {

/**
 * @brief 在线程和处理阶段之间移动一帧有效期控制权的独占 RAII 租约。
 *
 * 租约有效期间，FrameView 及其引用的像素缓冲区保持可用。租约释放时，
 * 通过归还回调释放共享存储，或将底层采集缓冲区归还给视频源。
 *
 * @htmlonly
 * <div class="vtb-resource-card">
 *   <div class="vtb-resource-card__title">独占归还责任</div>
 *   <span class="vtb-resource-owned">return_buffer_ / valid_</span> 共同表示一帧尚未
 *   完成的归还责任。析构或移动赋值覆盖旧值时调用回调；移动操作把责任转交给目标
 *   对象并使来源对象失效。<span class="vtb-resource-borrowed">view_</span> 只描述帧。
 * </div>
 * @endhtmlonly
 */
class DecodedFrameLease {
public:
    DecodedFrameLease() = default;
    DecodedFrameLease(FrameView view, std::function<void()> return_buffer);
    ~DecodedFrameLease();

    DecodedFrameLease(const DecodedFrameLease&) = delete;
    DecodedFrameLease& operator=(const DecodedFrameLease&) = delete;
    DecodedFrameLease(DecodedFrameLease&& other) noexcept;
    DecodedFrameLease& operator=(DecodedFrameLease&& other) noexcept;

    /**
     * @brief 取得当前租约关联的帧视图。
     * @return 帧格式、尺寸、时间戳及图像平面等只读信息。
     */
    const FrameView& view() const noexcept;

    /**
     * @brief 判断当前租约是否仍持有一帧有效数据。
     * @return 持有尚未归还的帧时返回 true，否则返回 false。
     */
    explicit operator bool() const noexcept;

private:
    /** @brief 归还当前帧并将租约重置为无效状态。 */
    void reset() noexcept;

    /** @brief <span class="vtb-resource-borrowed">借用资源</span> 帧元数据及平面视图。 */
    FrameView view_;

    /** @brief <span class="vtb-resource-owned">归还责任</span> 释放或归还底层帧缓冲区的回调。 */
    std::function<void()> return_buffer_;

    /** @brief <span class="vtb-resource-owned">所有权状态</span> 是否持有尚未归还的帧。 */
    bool valid_ = false;
};

}  // namespace vtb
