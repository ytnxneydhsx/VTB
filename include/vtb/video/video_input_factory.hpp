#pragma once

#include <memory>

#include "vtb/config/vtb_application_config.hpp"
#include "vtb/video/video_source.hpp"

namespace vtb {

class VideoInputFactory final {
public:
    std::unique_ptr<VideoSource> create(
        const VideoInputConfiguration& config) const;
};

}  // namespace vtb
