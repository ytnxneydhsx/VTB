#include "vtb/v4l2_camera.hpp"

#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <system_error>
#include <sys/ioctl.h>
#include <unistd.h>
#include <utility>

namespace vtb {
namespace {

[[noreturn]] void throw_errno(const std::string& operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

}  // namespace

V4l2Camera::V4l2Camera(CaptureConfig config) : config_(std::move(config)) {}

V4l2Camera::~V4l2Camera() {
    if (fd_ >= 0) {
        close(fd_);
    }
}

std::uint32_t V4l2Camera::open_and_configure() {
    if (fd_ >= 0) {
        throw std::logic_error("V4L2 camera is already open");
    }

    fd_ = open(config_.device_path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) {
        throw_errno("open " + config_.device_path);
    }

    v4l2_capability capability{};
    if (ioctl(fd_, VIDIOC_QUERYCAP, &capability) < 0) {
        throw_errno("VIDIOC_QUERYCAP");
    }

    std::uint32_t device_capabilities = capability.capabilities;
    if ((capability.capabilities & V4L2_CAP_DEVICE_CAPS) != 0) {
        device_capabilities = capability.device_caps;
    }

    if ((device_capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0) {
        throw std::runtime_error(config_.device_path +
                                 " does not support single-planar video capture");
    }
    if ((device_capabilities & V4L2_CAP_STREAMING) == 0) {
        throw std::runtime_error(config_.device_path +
                                 " does not support V4L2 streaming I/O");
    }

    v4l2_format format{};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = config_.width;
    format.fmt.pix.height = config_.height;
    format.fmt.pix.pixelformat = config_.pixel_format;
    format.fmt.pix.field = V4L2_FIELD_ANY;

    if (ioctl(fd_, VIDIOC_S_FMT, &format) < 0) {
        throw_errno("VIDIOC_S_FMT");
    }
    if (format.fmt.pix.pixelformat != config_.pixel_format) {
        throw std::runtime_error(
            config_.device_path + " rejected pixel format " +
            fourcc_to_string(config_.pixel_format) + " and selected " +
            fourcc_to_string(format.fmt.pix.pixelformat));
    }
    if (format.fmt.pix.width != config_.width ||
        format.fmt.pix.height != config_.height) {
        throw std::runtime_error(
            config_.device_path + " rejected resolution " +
            std::to_string(config_.width) + "x" +
            std::to_string(config_.height) + " and selected " +
            std::to_string(format.fmt.pix.width) + "x" +
            std::to_string(format.fmt.pix.height));
    }

    v4l2_streamparm stream_parameters{};
    stream_parameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd_, VIDIOC_G_PARM, &stream_parameters) < 0) {
        throw_errno("VIDIOC_G_PARM");
    }
    if ((stream_parameters.parm.capture.capability & V4L2_CAP_TIMEPERFRAME) ==
        0) {
        throw std::runtime_error(config_.device_path +
                                 " does not allow configuring the frame interval");
    }

    stream_parameters.parm.capture.timeperframe.numerator = 1;
    stream_parameters.parm.capture.timeperframe.denominator = config_.fps;
    if (ioctl(fd_, VIDIOC_S_PARM, &stream_parameters) < 0) {
        throw_errno("VIDIOC_S_PARM");
    }

    v4l2_streamparm actual_stream_parameters{};
    actual_stream_parameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd_, VIDIOC_G_PARM, &actual_stream_parameters) < 0) {
        throw_errno("VIDIOC_G_PARM after VIDIOC_S_PARM");
    }
    const std::uint32_t actual_numerator =
        actual_stream_parameters.parm.capture.timeperframe.numerator;
    const std::uint32_t actual_denominator =
        actual_stream_parameters.parm.capture.timeperframe.denominator;
    const std::uint64_t requested_denominator =
        static_cast<std::uint64_t>(config_.fps) * actual_numerator;
    if (actual_numerator == 0 ||
        actual_denominator != requested_denominator) {
        double actual_fps = 0.0;
        if (actual_numerator != 0) {
            actual_fps = static_cast<double>(actual_denominator) /
                static_cast<double>(actual_numerator);
        }
        throw std::runtime_error(
            config_.device_path + " rejected " +
            std::to_string(config_.fps) + " FPS and selected " +
            std::to_string(actual_fps) + " FPS");
    }

    return format.fmt.pix.bytesperline;
}

int V4l2Camera::fd() const noexcept {
    return fd_;
}

std::string fourcc_to_string(std::uint32_t fourcc) {
    std::string value(4, '\0');
    value[0] = static_cast<char>(fourcc & 0xffU);
    value[1] = static_cast<char>((fourcc >> 8U) & 0xffU);
    value[2] = static_cast<char>((fourcc >> 16U) & 0xffU);
    value[3] = static_cast<char>((fourcc >> 24U) & 0xffU);
    return value;
}

std::uint32_t fourcc_from_string(const std::string& value) {
    if (value.size() != 4U) {
        throw std::invalid_argument(
            "V4L2 pixel format must contain exactly four characters");
    }
    return static_cast<std::uint32_t>(
        static_cast<unsigned char>(value[0])) |
        (static_cast<std::uint32_t>(
             static_cast<unsigned char>(value[1]))
         << 8U) |
        (static_cast<std::uint32_t>(
             static_cast<unsigned char>(value[2]))
         << 16U) |
        (static_cast<std::uint32_t>(
             static_cast<unsigned char>(value[3]))
         << 24U);
}

}  // namespace vtb
