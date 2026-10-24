#include "vtb/v4l2_camera.hpp"

#include <cmath>
#include <cerrno>
#include <fcntl.h>
#include <iomanip>
#include <linux/videodev2.h>
#include <ostream>
#include <stdexcept>
#include <system_error>
#include <sys/ioctl.h>
#include <unistd.h>
#include <utility>

namespace vtb {

V4l2Camera::V4l2Camera(std::string device_path) : device_path_(std::move(device_path)) {}

V4l2Camera::~V4l2Camera() {
    if (fd_ >= 0) {
        close(fd_);
    }
}

void V4l2Camera::configure_mjpeg(std::uint32_t width,
                                 std::uint32_t height,
                                 std::uint32_t fps) {
    if (configured_) {
        throw std::logic_error("V4L2 camera is already configured");
    }
    if (width == 0 || height == 0 || fps == 0) {
        throw std::invalid_argument("width, height and fps must be greater than zero");
    }

    requested_width_ = width;
    requested_height_ = height;
    requested_fps_ = fps;

    open_device();
    query_capabilities();
    set_format();
    set_frame_rate();
    configured_ = true;
}

void V4l2Camera::print_configuration(std::ostream& output) const {
    if (!configured_) {
        throw std::logic_error("V4L2 camera is not configured");
    }

    output << "V4L2 device configured\n"
           << "  device: " << device_path_ << '\n'
           << "  driver: " << driver_ << '\n'
           << "  card: " << card_ << '\n'
           << "  bus: " << bus_info_ << '\n'
           << "  format: " << fourcc_to_string(actual_pixel_format_) << '\n'
           << "  resolution: " << actual_width_ << 'x' << actual_height_ << '\n'
           << "  fps: " << std::fixed << std::setprecision(2) << actual_fps() << '\n'
           << "  bytes per line: " << bytes_per_line_ << '\n'
           << "  size image: " << size_image_ << " bytes\n";
}

bool V4l2Camera::requested_configuration_was_accepted() const noexcept {
    return configured_ && actual_pixel_format_ == V4L2_PIX_FMT_MJPEG &&
           actual_width_ == requested_width_ && actual_height_ == requested_height_ &&
           std::abs(actual_fps() - requested_fps_) < 0.01;
}

int V4l2Camera::fd() const noexcept {
    return fd_;
}

void V4l2Camera::open_device() {
    if (fd_ >= 0) {
        throw std::logic_error("V4L2 camera is already open");
    }

    fd_ = open(device_path_.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) {
        throw_errno("open " + device_path_);
    }
}

void V4l2Camera::query_capabilities() {
    v4l2_capability capability{};
    if (ioctl_retry(fd_, VIDIOC_QUERYCAP, &capability) < 0) {
        throw_errno("VIDIOC_QUERYCAP");
    }

    const std::uint32_t device_capabilities =
        (capability.capabilities & V4L2_CAP_DEVICE_CAPS) != 0
            ? capability.device_caps
            : capability.capabilities;

    if ((device_capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0) {
        throw std::runtime_error(device_path_ +
                                 " does not support single-planar video capture");
    }
    if ((device_capabilities & V4L2_CAP_STREAMING) == 0) {
        throw std::runtime_error(device_path_ + " does not support V4L2 streaming I/O");
    }

    driver_ = c_string(capability.driver);
    card_ = c_string(capability.card);
    bus_info_ = c_string(capability.bus_info);
}

void V4l2Camera::set_format() {
    v4l2_format format{};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = requested_width_;
    format.fmt.pix.height = requested_height_;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    format.fmt.pix.field = V4L2_FIELD_ANY;

    if (ioctl_retry(fd_, VIDIOC_S_FMT, &format) < 0) {
        throw_errno("VIDIOC_S_FMT");
    }

    actual_pixel_format_ = format.fmt.pix.pixelformat;
    actual_width_ = format.fmt.pix.width;
    actual_height_ = format.fmt.pix.height;
    bytes_per_line_ = format.fmt.pix.bytesperline;
    size_image_ = format.fmt.pix.sizeimage;
}

void V4l2Camera::set_frame_rate() {
    v4l2_streamparm stream_parameters{};
    stream_parameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl_retry(fd_, VIDIOC_G_PARM, &stream_parameters) < 0) {
        throw_errno("VIDIOC_G_PARM");
    }
    if ((stream_parameters.parm.capture.capability & V4L2_CAP_TIMEPERFRAME) == 0) {
        throw std::runtime_error(device_path_ +
                                 " does not allow configuring the frame interval");
    }

    stream_parameters.parm.capture.timeperframe.numerator = 1;
    stream_parameters.parm.capture.timeperframe.denominator = requested_fps_;
    if (ioctl_retry(fd_, VIDIOC_S_PARM, &stream_parameters) < 0) {
        throw_errno("VIDIOC_S_PARM");
    }

    v4l2_streamparm actual_stream_parameters{};
    actual_stream_parameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl_retry(fd_, VIDIOC_G_PARM, &actual_stream_parameters) < 0) {
        throw_errno("VIDIOC_G_PARM after VIDIOC_S_PARM");
    }

    frame_interval_numerator_ =
        actual_stream_parameters.parm.capture.timeperframe.numerator;
    frame_interval_denominator_ =
        actual_stream_parameters.parm.capture.timeperframe.denominator;
}

int V4l2Camera::ioctl_retry(int fd, unsigned long request, void* argument) {
    int result;
    do {
        result = ioctl(fd, request, argument);
    } while (result < 0 && errno == EINTR);
    return result;
}

[[noreturn]] void V4l2Camera::throw_errno(const std::string& operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

std::string V4l2Camera::c_string(const unsigned char* value) {
    return reinterpret_cast<const char*>(value);
}

double V4l2Camera::actual_fps() const noexcept {
    if (frame_interval_numerator_ == 0) {
        return 0.0;
    }
    return static_cast<double>(frame_interval_denominator_) /
           static_cast<double>(frame_interval_numerator_);
}

std::string V4l2Camera::fourcc_to_string(std::uint32_t fourcc) {
    std::string value(4, '\0');
    value[0] = static_cast<char>(fourcc & 0xffU);
    value[1] = static_cast<char>((fourcc >> 8U) & 0xffU);
    value[2] = static_cast<char>((fourcc >> 16U) & 0xffU);
    value[3] = static_cast<char>((fourcc >> 24U) & 0xffU);
    return value;
}

}  // namespace vtb
