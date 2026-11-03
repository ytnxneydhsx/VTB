#pragma once

#include <cstdint>
#include <string>

#include <linux/videodev2.h>

namespace vtb {

class CaptureConfig {
public:
    std::string device_path = "/dev/video0";
    std::uint32_t pixel_format = V4L2_PIX_FMT_MJPEG;
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    std::uint32_t fps = 30;
};

}  // namespace vtb
