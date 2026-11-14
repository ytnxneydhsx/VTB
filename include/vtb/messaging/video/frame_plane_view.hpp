#pragma once

#include <cstddef>
#include <cstdint>

namespace vtb {

/**
 * @brief 单个图像平面的只读、非拥有内存视图。
 *
 * 一个平面可以通过 CPU 地址、DMA-BUF 文件描述符或两者同时访问。
 * 该类只描述访问方式，不负责释放像素内存或关闭文件描述符。
 *
 * @htmlonly
 * <div class="vtb-resource-card vtb-resource-card--borrowed">
 *   <div class="vtb-resource-card__title">借用资源：不负责释放</div>
 *   <span class="vtb-resource-borrowed">data_</span> 借用 MMAP/CPU 地址，
 *   <span class="vtb-resource-borrowed">dma_buf_fd_</span> 借用 DMA-BUF fd。
 *   析构、复制和移动均使用默认实现，不执行 munmap()、close() 或 QBUF。
 * </div>
 * @endhtmlonly
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
    /** @brief <span class="vtb-resource-borrowed">借用资源</span> 只读 CPU 起始地址。 */
    const std::byte* data_ = nullptr;

    /** @brief 该平面可访问区域的字节数。 */
    std::size_t size_bytes_ = 0;

    /** @brief 相邻两行起始地址之间的字节距离。 */
    std::uint32_t stride_bytes_ = 0;

    /** @brief <span class="vtb-resource-borrowed">借用资源</span> DMA-BUF fd，-1 表示不存在。 */
    int dma_buf_fd_ = -1;

    /** @brief 该平面在底层缓冲区中的字节偏移量。 */
    std::uint32_t offset_bytes_ = 0;
};

}  // namespace vtb
