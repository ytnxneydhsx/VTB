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

int ioctl_retry(int fd, unsigned long request, void* argument) {
    int result;
    do {
        result = ioctl(fd, request, argument);
    } while (result < 0 && errno == EINTR);
    return result;
}

[[noreturn]] void throw_errno(const std::string& operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

std::string c_string(const __u8* value) {
    return reinterpret_cast<const char*>(value);
}

}  // namespace

double NegotiatedCaptureConfig::fps() const {
    if (frame_interval_numerator == 0) {
        return 0.0;
    }
    return static_cast<double>(frame_interval_denominator) /
           static_cast<double>(frame_interval_numerator);
}

V4l2Camera::V4l2Camera(CaptureConfig config) : config_(std::move(config)) {}

V4l2Camera::~V4l2Camera() {
    if (fd_ >= 0) {
        close(fd_);
    }
}

NegotiatedCaptureConfig V4l2Camera::open_and_configure() {
    if (fd_ >= 0) {
        throw std::logic_error("V4L2 camera is already open");
    }

    fd_ = open(config_.device_path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) {
        throw_errno("open " + config_.device_path);
    }

    v4l2_capability capability{};
    if (ioctl_retry(fd_, VIDIOC_QUERYCAP, &capability) < 0) {
        throw_errno("VIDIOC_QUERYCAP");
    }

    const std::uint32_t device_capabilities =
        (capability.capabilities & V4L2_CAP_DEVICE_CAPS) != 0
            ? capability.device_caps
            : capability.capabilities;

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

    if (ioctl_retry(fd_, VIDIOC_S_FMT, &format) < 0) {
        throw_errno("VIDIOC_S_FMT");
    }

    v4l2_streamparm stream_parameters{};
    stream_parameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl_retry(fd_, VIDIOC_G_PARM, &stream_parameters) < 0) {
        throw_errno("VIDIOC_G_PARM");
    }
    if ((stream_parameters.parm.capture.capability & V4L2_CAP_TIMEPERFRAME) == 0) {
        throw std::runtime_error(config_.device_path +
                                 " does not allow configuring the frame interval");
    }

    stream_parameters.parm.capture.timeperframe.numerator = 1;
    stream_parameters.parm.capture.timeperframe.denominator = config_.fps;
    if (ioctl_retry(fd_, VIDIOC_S_PARM, &stream_parameters) < 0) {
        throw_errno("VIDIOC_S_PARM");
    }

    v4l2_streamparm actual_stream_parameters{};
    actual_stream_parameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl_retry(fd_, VIDIOC_G_PARM, &actual_stream_parameters) < 0) {
        throw_errno("VIDIOC_G_PARM after VIDIOC_S_PARM");
    }

    NegotiatedCaptureConfig actual;
    actual.driver = c_string(capability.driver);
    actual.card = c_string(capability.card);
    actual.bus_info = c_string(capability.bus_info);
    actual.pixel_format = format.fmt.pix.pixelformat;
    actual.width = format.fmt.pix.width;
    actual.height = format.fmt.pix.height;
    actual.bytes_per_line = format.fmt.pix.bytesperline;
    actual.size_image = format.fmt.pix.sizeimage;
    actual.frame_interval_numerator =
        actual_stream_parameters.parm.capture.timeperframe.numerator;
    actual.frame_interval_denominator =
        actual_stream_parameters.parm.capture.timeperframe.denominator;
    return actual;
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

}  // namespace vtb
