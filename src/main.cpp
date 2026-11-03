#include <exception>
#include <iostream>

#include "vtb/capture_config.hpp"
#include "vtb/v4l2_camera.hpp"

int main() {
    const vtb::CaptureConfig requested;

    try {
        vtb::V4l2Camera camera(requested);
        const std::uint32_t bytes_per_line = camera.open_and_configure();

        std::cout << "V4L2 device configured\n"
                  << "  device: " << requested.device_path << '\n'
                  << "  format: "
                  << vtb::fourcc_to_string(requested.pixel_format) << '\n'
                  << "  resolution: " << requested.width << 'x'
                  << requested.height << '\n'
                  << "  fps: " << requested.fps << '\n'
                  << "  bytes per line: " << bytes_per_line << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Camera configuration failed: " << error.what() << '\n';
        return 1;
    }
}
