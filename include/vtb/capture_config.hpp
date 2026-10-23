#pragma once

#include <cstdint>
#include <string>

#include <linux/videodev2.h>

namespace vtb {

struct CaptureConfig {
    std::string device_path = "/dev/video0";
    std::uint32_t pixel_format = V4L2_PIX_FMT_MJPEG;
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    std::uint32_t fps = 30;
};

struct NegotiatedCaptureConfig {
    std::string driver;
    std::string card;
    std::string bus_info;
    std::uint32_t pixel_format = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t bytes_per_line = 0;
    std::uint32_t size_image = 0;
    std::uint32_t frame_interval_numerator = 0;
    std::uint32_t frame_interval_denominator = 0;

    double fps() const;
};

}  // namespace vtb
