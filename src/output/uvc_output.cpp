#include "vtb/output/uvc_output.hpp"

#include <cerrno>
#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <system_error>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>

#include <linux/videodev2.h>

namespace vtb {
namespace {

[[noreturn]] void throw_system_error(const std::string& operation, int error) {
    throw std::system_error(error, std::generic_category(), operation);
}

void ioctl_checked(int fd, unsigned long request, void* argument,
                   const std::string& operation) {
    int result;
    do {
        result = ioctl(fd, request, argument);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        throw_system_error(operation, errno);
    }
}

void drain_eventfd(int fd) noexcept {
    if (fd < 0) {
        return;
    }
    std::uint64_t value = 0;
    while (read(fd, &value, sizeof(value)) == static_cast<ssize_t>(sizeof(value))) {
    }
}

bool wait_for_video(int video_fd, int stop_fd, int timeout_ms) {
    pollfd descriptors[2]{};
    descriptors[0].fd = video_fd;
    descriptors[0].events = POLLOUT | POLLPRI;
    descriptors[1].fd = stop_fd;
    descriptors[1].events = POLLIN;

    int result;
    do {
        result = poll(descriptors, 2, timeout_ms);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        throw_system_error("poll UVC output", errno);
    }
    if ((descriptors[1].revents & POLLIN) != 0) {
        throw std::runtime_error("UVC output stop requested");
    }
    if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        throw std::runtime_error("UVC output device disconnected or failed");
    }
    return result > 0 && (descriptors[0].revents & (POLLOUT | POLLPRI)) != 0;
}

std::string uppercase(std::string value) {
    for (char& character : value) {
        character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    }
    return value;
}

}  // namespace

UvcOutput::UvcOutput(UvcOutputConfig config) : config_(std::move(config)) {
    if (config_.device_path.empty()) {
        throw std::invalid_argument("UVC device path must not be empty");
    }
    if (config_.buffer_count < 2) {
        throw std::invalid_argument("UVC buffer count must be at least two");
    }
    if (config_.poll_timeout_ms <= 0) {
        throw std::invalid_argument("UVC poll timeout must be positive");
    }
    stop_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (stop_fd_ < 0) {
        throw_system_error("eventfd for UVC stop", errno);
    }
}

UvcOutput::~UvcOutput() {
    stop();
    if (stop_fd_ >= 0) {
        close(stop_fd_);
    }
}

bool UvcOutput::supports(const StreamDescriptor& descriptor) const {
    if (descriptor.type() != MediaType::video) {
        return false;
    }
    const std::string format = uppercase(descriptor.format());
    if (format == "MJPEG" || format == "MJPG") {
        return descriptor.encoded();
    }
    if (format == "YUYV") {
        return !descriptor.encoded();
    }
    return false;
}

void UvcOutput::start(const std::vector<StreamDescriptor>& descriptors) {
    if (fd_ >= 0) {
        throw std::logic_error("UVC output is already started");
    }
    if (descriptors.size() != 1 || !supports(descriptors.front())) {
        throw std::invalid_argument("UVC output requires one MJPEG or YUYV video stream");
    }
    const StreamDescriptor& descriptor = descriptors.front();
    descriptor.validate();
    drain_eventfd(stop_fd_);
    stop_requested_.store(false);

    try {
        fd_ = open(config_.device_path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd_ < 0) {
            throw_system_error("open " + config_.device_path, errno);
        }

        v4l2_capability capability{};
        ioctl_checked(fd_, VIDIOC_QUERYCAP, &capability, "VIDIOC_QUERYCAP");
        std::uint32_t caps = capability.capabilities;
        if ((caps & V4L2_CAP_DEVICE_CAPS) != 0) {
            caps = capability.device_caps;
        }
        if ((caps & V4L2_CAP_VIDEO_OUTPUT) == 0 ||
            (caps & V4L2_CAP_STREAMING) == 0) {
            throw std::runtime_error("UVC node lacks VIDEO_OUTPUT streaming capability");
        }

        v4l2_format format{};
        format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        format.fmt.pix.width = descriptor.width();
        format.fmt.pix.height = descriptor.height();
        if (uppercase(descriptor.format()) == "MJPEG") {
            format.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
        } else {
            format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        }
        format.fmt.pix.field = V4L2_FIELD_NONE;
        ioctl_checked(fd_, VIDIOC_S_FMT, &format, "VIDIOC_S_FMT UVC output");
        v4l2_format actual{};
        actual.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        ioctl_checked(fd_, VIDIOC_G_FMT, &actual, "VIDIOC_G_FMT UVC output");
        if (actual.fmt.pix.width != descriptor.width() ||
            actual.fmt.pix.height != descriptor.height() ||
            actual.fmt.pix.pixelformat != format.fmt.pix.pixelformat ||
            actual.fmt.pix.sizeimage == 0) {
            throw std::runtime_error("UVC driver adjusted the requested video format");
        }
        if (uppercase(descriptor.format()) == "YUYV" &&
            (descriptor.width() % 2 != 0 ||
             actual.fmt.pix.bytesperline != descriptor.width() * 2U)) {
            throw std::runtime_error(
                "UVC YUYV output requires tightly packed width*2 byte rows");
        }
        if (uppercase(descriptor.format()) == "YUYV") {
            const std::uint64_t packed_size =
                static_cast<std::uint64_t>(descriptor.width()) * descriptor.height() * 2U;
            if (actual.fmt.pix.sizeimage < packed_size) {
                throw std::runtime_error("UVC YUYV sizeimage is smaller than the packed frame");
            }
        }

        v4l2_streamparm parameters{};
        parameters.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        const int get_parameters_result = ioctl(fd_, VIDIOC_G_PARM, &parameters);
        if (get_parameters_result == 0 &&
            (parameters.parm.output.capability & V4L2_CAP_TIMEPERFRAME) != 0) {
            parameters.parm.output.timeperframe.numerator = descriptor.frame_rate_den();
            parameters.parm.output.timeperframe.denominator = descriptor.frame_rate_num();
            ioctl_checked(fd_, VIDIOC_S_PARM, &parameters, "VIDIOC_S_PARM UVC output");
            v4l2_streamparm actual_parameters{};
            actual_parameters.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
            ioctl_checked(fd_, VIDIOC_G_PARM, &actual_parameters,
                          "VIDIOC_G_PARM UVC output");
            if (actual_parameters.parm.output.timeperframe.numerator == 0 ||
                actual_parameters.parm.output.timeperframe.denominator == 0 ||
                static_cast<std::uint64_t>(actual_parameters.parm.output.timeperframe.numerator) *
                    descriptor.frame_rate_num() !=
                static_cast<std::uint64_t>(actual_parameters.parm.output.timeperframe.denominator) *
                    descriptor.frame_rate_den()) {
                throw std::runtime_error("UVC driver adjusted the requested frame rate");
            }
        } else if (get_parameters_result < 0 && errno != EINVAL && errno != ENOTTY) {
            throw_system_error("VIDIOC_G_PARM UVC output", errno);
        }

        pixel_format_ = actual.fmt.pix.pixelformat;
        width_ = actual.fmt.pix.width;
        height_ = actual.fmt.pix.height;
        size_image_ = actual.fmt.pix.sizeimage;
        stream_id_ = descriptor.id();

        v4l2_requestbuffers request{};
        request.count = config_.buffer_count;
        request.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        request.memory = V4L2_MEMORY_MMAP;
        ioctl_checked(fd_, VIDIOC_REQBUFS, &request, "VIDIOC_REQBUFS UVC output");
        if (request.count < 2) {
            throw std::runtime_error("UVC driver allocated fewer than two MMAP buffers");
        }
        mappings_.reserve(request.count);
        mapping_lengths_.reserve(request.count);
        queued_.assign(request.count, false);
        for (std::uint32_t index = 0; index < request.count; ++index) {
            v4l2_buffer buffer{};
            buffer.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
            buffer.memory = V4L2_MEMORY_MMAP;
            buffer.index = index;
            ioctl_checked(fd_, VIDIOC_QUERYBUF, &buffer, "VIDIOC_QUERYBUF UVC output");
            void* mapping = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE,
                                 MAP_SHARED, fd_, buffer.m.offset);
            if (mapping == MAP_FAILED) {
                throw_system_error("mmap UVC output buffer", errno);
            }
            mappings_.push_back(mapping);
            mapping_lengths_.push_back(buffer.length);
        }
    } catch (...) {
        stop();
        throw;
    }
}

void UvcOutput::write(MediaPacket packet) {
    if (stop_requested_.load()) {
        throw std::runtime_error("UVC output stop requested");
    }
    if (fd_ < 0 || stream_id_.empty()) {
        throw std::logic_error("UVC output is not started");
    }
    if (packet.stream_id() != stream_id_) {
        throw std::invalid_argument("packet belongs to a different UVC stream");
    }
    const auto& payload = *packet.payload();
    if (payload.empty() || payload.size() > size_image_) {
        throw std::invalid_argument("UVC frame size is outside the negotiated sizeimage");
    }
    if (pixel_format_ == V4L2_PIX_FMT_YUYV) {
        const std::size_t minimum_size =
            static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_) * 2U;
        if (payload.size() != minimum_size) {
            throw std::invalid_argument("YUYV frame size must match its packed image size");
        }
    }

    std::uint32_t index = static_cast<std::uint32_t>(queued_.size());
    for (std::uint32_t candidate = 0; candidate < queued_.size(); ++candidate) {
        if (!queued_[candidate]) {
            index = candidate;
            break;
        }
    }
    if (index == queued_.size()) {
        const bool ready = wait_for_video(fd_, stop_fd_, config_.poll_timeout_ms);
        if (!ready) {
            throw std::runtime_error("timed out waiting for a UVC output buffer");
        }
        v4l2_buffer completed{};
        completed.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        completed.memory = V4L2_MEMORY_MMAP;
        const int result = ioctl(fd_, VIDIOC_DQBUF, &completed);
        if (result < 0) {
            if (errno == EAGAIN) {
                throw std::runtime_error("UVC output readiness had no completed buffer");
            }
            throw_system_error("VIDIOC_DQBUF UVC output", errno);
        }
        if (completed.index >= queued_.size()) {
            throw std::runtime_error("UVC driver returned an invalid buffer index");
        }
        queued_[completed.index] = false;
        index = completed.index;
    }

    if (payload.size() > mapping_lengths_[index]) {
        throw std::invalid_argument("UVC frame exceeds mapped buffer length");
    }
    std::memcpy(mappings_[index], payload.data(), payload.size());
    v4l2_buffer buffer{};
    buffer.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = index;
    buffer.bytesused = static_cast<__u32>(payload.size());
    buffer.field = V4L2_FIELD_NONE;
    buffer.timestamp.tv_sec = static_cast<__s64>(packet.pts_ns() / 1000000000ULL);
    buffer.timestamp.tv_usec = static_cast<__s64>((packet.pts_ns() % 1000000000ULL) / 1000ULL);
    ioctl_checked(fd_, VIDIOC_QBUF, &buffer, "VIDIOC_QBUF UVC output");
    queued_[index] = true;

    if (!streaming_) {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        ioctl_checked(fd_, VIDIOC_STREAMON, &type, "VIDIOC_STREAMON UVC output");
        streaming_ = true;
    }
}

void UvcOutput::request_stop() noexcept {
    stop_requested_.store(true);
    if (stop_fd_ >= 0) {
        const std::uint64_t value = 1;
        const ssize_t ignored = ::write(stop_fd_, &value, sizeof(value));
        (void)ignored;
    }
}

void UvcOutput::stop() noexcept {
    if (fd_ >= 0 && streaming_) {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        ioctl(fd_, VIDIOC_STREAMOFF, &type);
        streaming_ = false;
    }
    for (std::size_t index = 0; index < mappings_.size(); ++index) {
        void* mapping = mappings_[index];
        if (mapping != MAP_FAILED && mapping != nullptr) {
            if (index < mapping_lengths_.size()) {
                munmap(mapping, mapping_lengths_[index]);
            }
        }
    }
    mappings_.clear();
    mapping_lengths_.clear();
    queued_.clear();
    if (fd_ >= 0) {
        v4l2_requestbuffers request{};
        request.count = 0;
        request.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        request.memory = V4L2_MEMORY_MMAP;
        ioctl(fd_, VIDIOC_REQBUFS, &request);
        close(fd_);
        fd_ = -1;
    }
    stream_id_.clear();
    drain_eventfd(stop_fd_);
}

std::string UvcOutput::name() const { return "uvc"; }

}  // namespace vtb
