#pragma once

#include "vtb/face/model_input.hpp"
#include "vtb/video/video_stream_control_protocol.hpp"

namespace vtb {

class ImageTransformer {
public:
    ImageTransformer() = default;
    virtual ~ImageTransformer() = default;

    ImageTransformer(const ImageTransformer&) = delete;
    ImageTransformer& operator=(const ImageTransformer&) = delete;
    ImageTransformer(ImageTransformer&&) = delete;
    ImageTransformer& operator=(ImageTransformer&&) = delete;

    /** @brief 开始一代视频源；无源相关缓存的实现可以沿用默认空操作。 */
    virtual void begin_source(VideoStreamGeneration generation);

    // The first RKNN path scales the source crop directly to the model size.
    // Implementations must store the exact source crop in ModelInputView and
    // must not add unreported letterboxing or padding.
    virtual ModelInputLease prepare(
        const FrameView& frame,
        const CropRegion& crop,
        const ModelInputSpec& spec) = 0;

    /** @brief 退休一代视频源；默认实现不持有源相关资源。 */
    virtual void retire_source(VideoStreamGeneration generation) noexcept;
};

}  // namespace vtb
