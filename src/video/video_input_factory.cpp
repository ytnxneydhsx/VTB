#include "vtb/video/video_input_factory.hpp"

#include <stdexcept>
#include <utility>

#include "vtb/capture_config.hpp"
#include "vtb/v4l2_camera.hpp"
#include "vtb/video/opencv_video_source.hpp"
#include "vtb/video/v4l2_device_validator.hpp"

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

    V4l2DeviceValidator validator;
    validator.validate(requested);

    OpenCvVideoSourceConfig source_config;
    source_config.device_path = requested.device_path;
    source_config.pixel_format = requested.pixel_format;
    source_config.width = requested.width;
    source_config.height = requested.height;
    source_config.fps = requested.fps;
    source_config.buffer_count = config.buffer_count;
    return std::make_unique<OpenCvVideoSource>(std::move(source_config));
}

}  // namespace vtb
