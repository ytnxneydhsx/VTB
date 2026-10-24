#include <cmath>
#include <exception>
#include <iomanip>
#include <iostream>

#include "vtb/capture_config.hpp"
#include "vtb/v4l2_camera.hpp"

int main() {
    const vtb::CaptureConfig requested;

    try {
        vtb::V4l2Camera camera(requested);
        const vtb::NegotiatedCaptureConfig actual = camera.open_and_configure();

        std::cout << "V4L2 device configured\n"
                  << "  device: " << requested.device_path << '\n'
                  << "  driver: " << actual.driver << '\n'
                  << "  card: " << actual.card << '\n'
                  << "  bus: " << actual.bus_info << '\n'
                  << "  format: " << vtb::fourcc_to_string(actual.pixel_format) << '\n'
                  << "  resolution: " << actual.width << 'x' << actual.height << '\n'
                  << "  fps: " << std::fixed << std::setprecision(2) << actual.fps() << '\n'
                  << "  bytes per line: " << actual.bytes_per_line << '\n'
                  << "  size image: " << actual.size_image << " bytes\n";

        const bool format_matches = actual.pixel_format == requested.pixel_format;
        const bool resolution_matches =
            actual.width == requested.width && actual.height == requested.height;
        const bool fps_matches = std::abs(actual.fps() - requested.fps) < 0.01;

        if (!format_matches || !resolution_matches || !fps_matches) {
            std::cerr << "The driver adjusted the requested capture configuration.\n";
            return 2;
        }

        std::cout << "Requested MJPG 1280x720 @ 30 fps was accepted.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Camera configuration failed: " << error.what() << '\n';
        return 1;
    }
}
