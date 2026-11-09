#pragma once

#include "vtb/face/image_transformer.hpp"

namespace vtb {

/**
 * @brief 使用 CPU 准备模型输入的 ImageTransformer 实现。
 *
 * 该实现读取 FrameView 的 CPU 地址，完成 ROI 裁剪、双线性缩放以及
 * RGB/BGR/YUYV/NV12/NV21 到 RGB888 的转换。它保留用于功能对照和
 * 不依赖 Rockchip RGA 的构建，不会在 RGA 失败时被自动选中。
 */
class CpuImageTransformer final : public ImageTransformer {
public:
    CpuImageTransformer() = default;
    ~CpuImageTransformer() override = default;

    CpuImageTransformer(const CpuImageTransformer&) = delete;
    CpuImageTransformer& operator=(const CpuImageTransformer&) = delete;
    CpuImageTransformer(CpuImageTransformer&&) = delete;
    CpuImageTransformer& operator=(CpuImageTransformer&&) = delete;

    ModelInputLease prepare(
        const FrameView& frame,
        const CropRegion& crop,
        const ModelInputSpec& spec) override;
};

}  // namespace vtb
