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
 * 配置指定的 DMA heap 分配并映射到 CPU 地址，供当前 RKNN Runtime 输入接口读取。
 */
class RgaImageTransformer final : public ImageTransformer {
public:
    explicit RgaImageTransformer(std::string dma_heap_path);
    ~RgaImageTransformer() override;

    RgaImageTransformer(const RgaImageTransformer&) = delete;
    RgaImageTransformer& operator=(const RgaImageTransformer&) = delete;
    RgaImageTransformer(RgaImageTransformer&&) = delete;
    RgaImageTransformer& operator=(RgaImageTransformer&&) = delete;

    ModelInputLease prepare(
        const FrameView& frame,
        const CropRegion& crop,
        const ModelInputSpec& spec) override;

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
