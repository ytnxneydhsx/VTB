#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace vtb {

enum class VideoPixelFormat {
    unknown,
    yuyv,
    nv12,
    nv21,
    rgb888,
    bgr888,
};

enum class FrameMemoryType {
    cpu,
    dma_buf,
};

enum class FrameTimestampSource {
    driver_monotonic,
    dequeue_monotonic,
    file_timeline,
};

class FramePlaneView {
public:
    FramePlaneView() = default;
    FramePlaneView(
        const std::byte* data,
        std::size_t size_bytes,
        std::uint32_t stride_bytes,
        int dma_buf_fd,
        std::uint32_t offset_bytes);
    ~FramePlaneView() = default;

    FramePlaneView(const FramePlaneView&) = default;
    FramePlaneView& operator=(const FramePlaneView&) = default;
    FramePlaneView(FramePlaneView&&) noexcept = default;
    FramePlaneView& operator=(FramePlaneView&&) noexcept = default;

    const std::byte* data() const noexcept;
    std::size_t size_bytes() const noexcept;
    std::uint32_t stride_bytes() const noexcept;
    int dma_buf_fd() const noexcept;
    std::uint32_t offset_bytes() const noexcept;

private:
    const std::byte* data_ = nullptr;
    std::size_t size_bytes_ = 0;
    std::uint32_t stride_bytes_ = 0;
    int dma_buf_fd_ = -1;
    std::uint32_t offset_bytes_ = 0;
};

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
    std::array<FramePlaneView, max_planes> planes_{};
    std::size_t plane_count_ = 0;
};

class DecodedFrameLease {
public:
    DecodedFrameLease() = default;
    DecodedFrameLease(FrameView view, std::function<void()> return_buffer);
    ~DecodedFrameLease();

    DecodedFrameLease(const DecodedFrameLease&) = delete;
    DecodedFrameLease& operator=(const DecodedFrameLease&) = delete;
    DecodedFrameLease(DecodedFrameLease&& other) noexcept;
    DecodedFrameLease& operator=(DecodedFrameLease&& other) noexcept;

    const FrameView& view() const noexcept;
    explicit operator bool() const noexcept;

private:
    void reset() noexcept;

    FrameView view_;
    std::function<void()> return_buffer_;
    bool valid_ = false;
};

}  // namespace vtb
