#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "vtb/messaging/video/frame_plane_view.hpp"
#include "vtb/messaging/video/video_frame_types.hpp"

namespace vtb {

/**
 * @brief 描述一张帧的格式、尺寸、时间戳和图像平面。
 *
 * FrameView 只组合普通元数据与 FramePlaneView。它可以安全地按值复制或移动，
 * 但这些操作只复制借用入口，不会延长底层像素缓冲区的生命周期。
 *
 * @htmlonly
 * <div class="vtb-resource-card vtb-resource-card--borrowed">
 *   <div class="vtb-resource-card__title">借用资源：有效期由帧租约保证</div>
 *   <span class="vtb-resource-borrowed">planes_</span> 包含一个或多个非拥有
 *   FramePlaneView。访问 FrameView 时必须同时持有对应的 DecodedFrameLease。
 * </div>
 * @endhtmlonly
 */
class FrameView {
public:
    static constexpr std::size_t max_planes = 3;

    FrameView() = default;
    FrameView(
        VideoPixelFormat pixel_format,
        FrameMemoryType memory_type,
        std::uint32_t width,
        std::uint32_t height,
        std::uint64_t frame_id,
        std::uint64_t timestamp_ns,
        FrameTimestampSource timestamp_source,
        std::array<FramePlaneView, max_planes> planes,
        std::size_t plane_count);
    ~FrameView() = default;

    FrameView(const FrameView&) = default;
    FrameView& operator=(const FrameView&) = default;
    FrameView(FrameView&&) noexcept = default;
    FrameView& operator=(FrameView&&) noexcept = default;

    VideoPixelFormat pixel_format() const noexcept;
    FrameMemoryType memory_type() const noexcept;
    std::uint32_t width() const noexcept;
    std::uint32_t height() const noexcept;
    std::uint64_t frame_id() const noexcept;
    std::uint64_t timestamp_ns() const noexcept;
    FrameTimestampSource timestamp_source() const noexcept;
    std::size_t plane_count() const noexcept;
    const FramePlaneView& plane(std::size_t index) const;

private:
    VideoPixelFormat pixel_format_ = VideoPixelFormat::unknown;
    FrameMemoryType memory_type_ = FrameMemoryType::cpu;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint64_t frame_id_ = 0;
    std::uint64_t timestamp_ns_ = 0;
    FrameTimestampSource timestamp_source_ =
        FrameTimestampSource::dequeue_monotonic;
    /** @brief <span class="vtb-resource-borrowed">借用资源</span> 图像平面视图集合。 */
    std::array<FramePlaneView, max_planes> planes_{};
    std::size_t plane_count_ = 0;
};

}  // namespace vtb
