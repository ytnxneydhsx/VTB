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

/**
 * @brief 单个图像平面的只读、非拥有内存视图。
 *
 * 一个平面可以通过 CPU 地址、DMA-BUF 文件描述符或两者同时访问。
 * 该类只描述访问方式，不负责释放像素内存或关闭文件描述符。
 */
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

    /**
     * @brief 取得该平面的只读 CPU 起始地址。
     * @return 存在 CPU 映射时返回起始地址，否则返回 nullptr。
     */
    const std::byte* data() const noexcept;

    /**
     * @brief 取得该平面可访问区域的字节数。
     * @return 平面的有效字节数。
     */
    std::size_t size_bytes() const noexcept;

    /**
     * @brief 取得相邻两行起始地址之间的字节距离。
     * @return 包含行尾填充的行步长，单位为字节。
     */
    std::uint32_t stride_bytes() const noexcept;

    /**
     * @brief 取得该平面借用的 DMA-BUF 文件描述符。
     * @return 存在 DMA-BUF 时返回非负文件描述符，否则返回 -1。
     */
    int dma_buf_fd() const noexcept;

    /**
     * @brief 取得该平面在底层缓冲区中的起始偏移。
     * @return 从底层缓冲区起点开始计算的字节偏移量。
     */
    std::uint32_t offset_bytes() const noexcept;

private:
    /** @brief 该平面借用的只读 CPU 起始地址。 */
    const std::byte* data_ = nullptr;

    /** @brief 该平面可访问区域的字节数。 */
    std::size_t size_bytes_ = 0;

    /** @brief 相邻两行起始地址之间的字节距离。 */
    std::uint32_t stride_bytes_ = 0;

    /** @brief 该平面借用的 DMA-BUF 文件描述符，-1 表示不存在。 */
    int dma_buf_fd_ = -1;

    /** @brief 该平面在底层缓冲区中的字节偏移量。 */
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

/**
 * @brief 管理一帧解码视频数据有效期的独占 RAII 租约。
 *
 * 租约有效期间，FrameView 及其引用的像素缓冲区保持可用。租约释放时，
 * 通过归还回调释放共享存储，或将底层采集缓冲区归还给视频源。
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
    /**
     * @brief 归还当前帧并将租约重置为无效状态。
     */
    void reset() noexcept;

    /** @brief 当前帧的元数据及各图像平面的非拥有视图。 */
    FrameView view_;

    /** @brief 租约释放时用于释放或归还底层帧缓冲区的回调。 */
    std::function<void()> return_buffer_;

    /** @brief 是否持有尚未归还的有效帧。 */
    bool valid_ = false;
};

}  // namespace vtb
