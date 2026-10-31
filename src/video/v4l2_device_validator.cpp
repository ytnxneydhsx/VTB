#include "vtb/video/v4l2_device_validator.hpp"

#include <stdexcept>
#include <string>

#include "vtb/v4l2_camera.hpp"

namespace vtb {

NegotiatedCaptureConfig V4l2DeviceValidator::validate(
    const CaptureConfig& requested) const {
    V4l2Camera camera(requested);
    NegotiatedCaptureConfig actual = camera.open_and_configure();

    if (actual.pixel_format != requested.pixel_format) {
        throw std::runtime_error(
            requested.device_path + " rejected pixel format " +
            fourcc_to_string(requested.pixel_format) + " and selected " +
            fourcc_to_string(actual.pixel_format));
    }
    if (actual.width != requested.width || actual.height != requested.height) {
        throw std::runtime_error(
            requested.device_path + " rejected resolution " +
            std::to_string(requested.width) + "x" +
            std::to_string(requested.height) + " and selected " +
            std::to_string(actual.width) + "x" +
            std::to_string(actual.height));
    }
    const std::uint64_t requested_scaled =
        static_cast<std::uint64_t>(requested.fps) *
        actual.frame_interval_numerator;
    if (actual.frame_interval_numerator == 0 ||
        actual.frame_interval_denominator != requested_scaled) {
        throw std::runtime_error(
            requested.device_path + " rejected " +
            std::to_string(requested.fps) + " FPS and selected " +
            std::to_string(actual.fps()) + " FPS");
    }
    return actual;
}

}  // namespace vtb
