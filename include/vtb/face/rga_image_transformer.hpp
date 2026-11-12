#pragma once

#include <memory>
#include <string>

#include "vtb/face/image_transformer.hpp"

namespace vtb {

/**
 * @brief 使用 RK3588 RGA 准备 RKNN 模型输入。
 *
 * 输入帧必须带有可导入 RGA 的 DMA-BUF fd。prepare() 在一次同步 RGA
 * 操作中完成源图 ROI 裁剪、缩放和 YUV 到 RGB888 的转换。输出缓冲区由
 * 配置指定的 DMA heap 分配，并通过 ModelInputView 同时交付 CPU 地址和
 * DMA-BUF fd。YUYV、NV12 和 NV21 的有效 ROI 会扩展到驱动要求的偶数坐标
 * 和尺寸，并把实际 ROI 交给模型后处理。RGA 实现不创建或配置 RKNN context；
 * 模型消费者自行导入缓冲区。
 *
 * 同一源代次中首次看到某个摄像头 DMA-BUF 时，转换器复制 fd、导入 RGA handle 并
 * 缓存完整源缓冲区描述；后续帧复用该 handle。retire_source() 在消费线程确认该代
 * 已无在途帧后统一释放输入 handle 和复制的 fd。输出模型缓冲区继续按规格复用。
 */
class RgaImageTransformer final : public ImageTransformer {
public:
    explicit RgaImageTransformer(std::string dma_heap_path);
    ~RgaImageTransformer() override;

    RgaImageTransformer(const RgaImageTransformer&) = delete;
    RgaImageTransformer& operator=(const RgaImageTransformer&) = delete;
    RgaImageTransformer(RgaImageTransformer&&) = delete;
    RgaImageTransformer& operator=(RgaImageTransformer&&) = delete;

    void begin_source(VideoStreamGeneration generation) override;
    ModelInputLease prepare(
        const FrameView& frame,
        const CropRegion& crop,
        const ModelInputSpec& spec) override;
    void retire_source(VideoStreamGeneration generation) noexcept override;

private:
    /**
     * @brief 隐藏 DMA heap、mmap 和 RGA handle 等平台资源。
     *
     * Impl 与 RgaImageTransformer 生命周期一致，仅用于保持公开头文件不依赖
     * Linux DMA-BUF 和 Rockchip RGA 头文件。
     */
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vtb
