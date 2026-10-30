#pragma once

#include "vtb/face/image_transformer.hpp"

namespace vtb {

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
