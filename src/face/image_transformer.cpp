#include "vtb/face/image_transformer.hpp"

namespace vtb {

void ImageTransformer::begin_source(VideoStreamGeneration generation) {
    static_cast<void>(generation);
}

void ImageTransformer::retire_source(
    VideoStreamGeneration generation) noexcept {
    static_cast<void>(generation);
}

}  // namespace vtb
