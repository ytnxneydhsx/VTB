#include <exception>
#include <iostream>

#include "vtb/v4l2_camera.hpp"

int main() {
    try {
        vtb::V4l2Camera camera("/dev/video0");
        camera.configure_mjpeg(1280, 720, 30);
        camera.print_configuration(std::cout);

        if (!camera.requested_configuration_was_accepted()) {
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
