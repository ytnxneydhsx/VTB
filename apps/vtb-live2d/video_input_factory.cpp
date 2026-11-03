#include "video_input_factory.hpp"

#include <stdexcept>
#include <utility>

#include "vtb/capture_config.hpp"
#include "vtb/v4l2_camera.hpp"
#include "vtb/video/camera_source.hpp"

namespace vtb {

std::unique_ptr<VideoSource> VideoInputFactory::create(
    const VideoInputConfiguration& config) const {
    if (config.type != VideoInputType::v4l2_camera) {
        throw std::invalid_argument("Unsupported video input type");
    }

    CaptureConfig requested;
    requested.device_path = config.device;
    requested.pixel_format = fourcc_from_string(config.pixel_format);
    requested.width = config.width;
    requested.height = config.height;
    requested.fps = config.fps;

    return std::make_unique<CameraSource>(
        std::move(requested), config.buffer_count);
}

}  // namespace vtb
