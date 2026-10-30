#pragma once

#include "vtb/face/model_input.hpp"

namespace vtb {

class ImageTransformer {
public:
    ImageTransformer() = default;
    virtual ~ImageTransformer() = default;

    ImageTransformer(const ImageTransformer&) = delete;
    ImageTransformer& operator=(const ImageTransformer&) = delete;
    ImageTransformer(ImageTransformer&&) = delete;
    ImageTransformer& operator=(ImageTransformer&&) = delete;

    // The first RKNN path scales the source crop directly to the model size.
    // Implementations must store the exact source crop in ModelInputView and
    // must not add unreported letterboxing or padding.
    virtual ModelInputLease prepare(
        const FrameView& frame,
        const CropRegion& crop,
        const ModelInputSpec& spec) = 0;
};

}  // namespace vtb
