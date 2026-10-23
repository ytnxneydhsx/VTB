#include "vtb/v4l2_capture.hpp"

#include <linux/videodev2.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>

namespace vtb {
namespace {

int retryIoctl(const int fd, const unsigned long request, void* argument) {
    int result = 0;
    do {
        result = ::ioctl(fd, request, argument);
    } while (result == -1 && errno == EINTR);
    return result;
}

std::runtime_error systemError(const std::string& operation) {
    return std::runtime_error(operation + ": " + std::strerror(errno));
}

std::int64_t timevalToNanoseconds(const timeval& timestamp) {
    constexpr std::int64_t nanoseconds_per_second = 1'000'000'000;
    constexpr std::int64_t nanoseconds_per_microsecond = 1'000;
    return static_cast<std::int64_t>(timestamp.tv_sec) * nanoseconds_per_second +
           static_cast<std::int64_t>(timestamp.tv_usec) * nanoseconds_per_microsecond;
}

}  // namespace

V4L2Capture::V4L2Capture(CaptureConfig config, FrameRingBuffer& output)
    : config_(std::move(config)), output_(output) {
    if (config_.pixel_format == 0) {
        config_.pixel_format = V4L2_PIX_FMT_MJPEG;
    }
    if (config_.width == 0 || config_.height == 0 || config_.fps == 0) {
        throw std::invalid_argument("width, height and fps must be greater than zero");
    }
    if (config_.driver_buffer_count < 2) {
        throw std::invalid_argument("at least two V4L2 driver buffers are required");
    }
}

V4L2Capture::~V4L2Capture() {
    cleanup();
}

void V4L2Capture::run(const std::atomic_bool& stop_requested) {
    openDevice();
    configureDevice();
    mapBuffers();
    queueBuffers();
    startStreaming();

    std::uint64_t captured_frames = 0;
    while (!stop_requested.load(std::memory_order_relaxed)) {
        pollfd descriptor{};
        descriptor.fd = fd_;
        descriptor.events = POLLIN | POLLPRI;

        const int poll_result = ::poll(&descriptor, 1, 500);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw systemError("poll camera");
        }
        if (poll_result == 0) {
            continue;
        }
        if ((descriptor.revents & POLLNVAL) != 0) {
            throw std::runtime_error("camera file descriptor became invalid");
        }

        v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        if (retryIoctl(fd_, VIDIOC_DQBUF, &buffer) == -1) {
            if (errno == EAGAIN) {
                continue;
            }
            throw systemError("VIDIOC_DQBUF");
        }

        if (buffer.index >= mapped_buffers_.size()) {
            throw std::runtime_error("driver returned an invalid buffer index");
        }
        const MappedBuffer& mapped = mapped_buffers_[buffer.index];
        if (buffer.bytesused > mapped.length) {
            throw std::runtime_error("driver reported a frame larger than its buffer");
        }

        FrameMetadata metadata;
        metadata.driver_sequence = buffer.sequence;
        metadata.timestamp_ns = timevalToNanoseconds(buffer.timestamp);
        metadata.v4l2_flags = buffer.flags;
        metadata.pixel_format = negotiated_.pixel_format;
        metadata.width = negotiated_.width;
        metadata.height = negotiated_.height;
        output_.push(metadata, mapped.address, buffer.bytesused);

        if (retryIoctl(fd_, VIDIOC_QBUF, &buffer) == -1) {
            throw systemError("VIDIOC_QBUF");
        }

        ++captured_frames;
        if (config_.max_frames > 0 && captured_frames >= config_.max_frames) {
            break;
        }
    }

    stopStreaming();
}

const NegotiatedFormat& V4L2Capture::negotiatedFormat() const noexcept {
    return negotiated_;
}

void V4L2Capture::openDevice() {
    fd_ = ::open(config_.device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd_ == -1) {
        throw systemError("open " + config_.device);
    }

    v4l2_capability capability{};
    if (retryIoctl(fd_, VIDIOC_QUERYCAP, &capability) == -1) {
        throw systemError("VIDIOC_QUERYCAP");
    }

    const std::uint32_t capabilities =
        (capability.capabilities & V4L2_CAP_DEVICE_CAPS) != 0
            ? capability.device_caps
            : capability.capabilities;
    if ((capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0) {
        throw std::runtime_error(config_.device + " is not a video capture node");
    }
    if ((capabilities & V4L2_CAP_STREAMING) == 0) {
        throw std::runtime_error(config_.device + " does not support streaming I/O");
    }
}

void V4L2Capture::configureDevice() {
    v4l2_format format{};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = config_.width;
    format.fmt.pix.height = config_.height;
    format.fmt.pix.pixelformat = config_.pixel_format;
    format.fmt.pix.field = V4L2_FIELD_ANY;

    if (retryIoctl(fd_, VIDIOC_S_FMT, &format) == -1) {
        throw systemError("VIDIOC_S_FMT");
    }

    negotiated_.width = format.fmt.pix.width;
    negotiated_.height = format.fmt.pix.height;
    negotiated_.pixel_format = format.fmt.pix.pixelformat;
    negotiated_.size_image = format.fmt.pix.sizeimage;

    v4l2_streamparm parameters{};
    parameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parameters.parm.capture.timeperframe.numerator = 1;
    parameters.parm.capture.timeperframe.denominator = config_.fps;
    if (retryIoctl(fd_, VIDIOC_S_PARM, &parameters) == -1) {
        throw systemError("VIDIOC_S_PARM");
    }
    negotiated_.fps_numerator =
        parameters.parm.capture.timeperframe.numerator;
    negotiated_.fps_denominator =
        parameters.parm.capture.timeperframe.denominator;
}

void V4L2Capture::mapBuffers() {
    v4l2_requestbuffers request{};
    request.count = config_.driver_buffer_count;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    if (retryIoctl(fd_, VIDIOC_REQBUFS, &request) == -1) {
        throw systemError("VIDIOC_REQBUFS");
    }
    if (request.count < 2) {
        throw std::runtime_error("camera allocated fewer than two V4L2 buffers");
    }

    mapped_buffers_.reserve(request.count);
    for (std::uint32_t index = 0; index < request.count; ++index) {
        v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        if (retryIoctl(fd_, VIDIOC_QUERYBUF, &buffer) == -1) {
            throw systemError("VIDIOC_QUERYBUF");
        }

        void* address = ::mmap(
            nullptr,
            buffer.length,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            fd_,
            static_cast<off_t>(buffer.m.offset));
        if (address == MAP_FAILED) {
            throw systemError("mmap camera buffer");
        }
        mapped_buffers_.push_back({address, buffer.length});
    }
}

void V4L2Capture::queueBuffers() {
    for (std::uint32_t index = 0; index < mapped_buffers_.size(); ++index) {
        v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        if (retryIoctl(fd_, VIDIOC_QBUF, &buffer) == -1) {
            throw systemError("initial VIDIOC_QBUF");
        }
    }
}

void V4L2Capture::startStreaming() {
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (retryIoctl(fd_, VIDIOC_STREAMON, &type) == -1) {
        throw systemError("VIDIOC_STREAMON");
    }
    streaming_ = true;
}

void V4L2Capture::stopStreaming() noexcept {
    if (!streaming_ || fd_ == -1) {
        return;
    }
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    retryIoctl(fd_, VIDIOC_STREAMOFF, &type);
    streaming_ = false;
}

void V4L2Capture::cleanup() noexcept {
    stopStreaming();
    for (const MappedBuffer& buffer : mapped_buffers_) {
        if (buffer.address != nullptr && buffer.address != MAP_FAILED) {
            ::munmap(buffer.address, buffer.length);
        }
    }
    mapped_buffers_.clear();
    if (fd_ != -1) {
        ::close(fd_);
        fd_ = -1;
    }
}

std::uint32_t makeFourcc(const std::string& text) {
    if (text.size() != 4) {
        throw std::invalid_argument("pixel format must contain exactly four characters");
    }
    return v4l2_fourcc(text[0], text[1], text[2], text[3]);
}

std::string fourccToString(const std::uint32_t fourcc) {
    std::string text(4, ' ');
    text[0] = static_cast<char>(fourcc & 0xffU);
    text[1] = static_cast<char>((fourcc >> 8U) & 0xffU);
    text[2] = static_cast<char>((fourcc >> 16U) & 0xffU);
    text[3] = static_cast<char>((fourcc >> 24U) & 0xffU);
    return text;
}

}  // namespace vtb
