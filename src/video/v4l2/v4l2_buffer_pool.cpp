#include "vtb/video/v4l2/v4l2_buffer_pool.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <utility>

#include <linux/videodev2.h>

#include "vtb/time/monotonic_clock.hpp"
#include "vtb/v4l2_camera.hpp"

namespace vtb {
namespace {

[[noreturn]] void throw_errno(const std::string& operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

std::uint64_t timeval_to_ns(const timeval& timestamp) noexcept {
    return static_cast<std::uint64_t>(timestamp.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(timestamp.tv_usec) * 1000ULL;
}

VideoPixelFormat to_video_pixel_format(std::uint32_t fourcc) {
    if (fourcc == V4L2_PIX_FMT_YUYV) {
        return VideoPixelFormat::yuyv;
    }
    if (fourcc == V4L2_PIX_FMT_NV12) {
        return VideoPixelFormat::nv12;
    }
    if (fourcc == V4L2_PIX_FMT_NV21) {
        return VideoPixelFormat::nv21;
    }
    throw std::runtime_error(
        "CameraSource currently supports decoded YUYV, NV12 and NV21 frames; "
        "negotiated " +
        fourcc_to_string(fourcc) +
        " requires the planned MPP decoder path");
}

}  // namespace

V4l2BufferPool::RequeueOnLeaseRelease::RequeueOnLeaseRelease(
    std::shared_ptr<V4l2BufferPool> owner,
    std::uint32_t index) noexcept
    : owner_(std::move(owner)), index_(index) {}

void V4l2BufferPool::RequeueOnLeaseRelease::operator()() const noexcept {
    owner_->requeue(index_);
}

V4l2BufferPool::V4l2BufferPool(
    CaptureConfig requested,
    std::uint32_t requested_buffer_count)
    : requested_(std::move(requested)),
      camera_(requested_) {
    to_video_pixel_format(requested_.pixel_format);
    try {
        bytes_per_line_ = camera_.open_and_configure();
        initialize(requested_buffer_count);
    } catch (...) {
        shutdown();
        throw;
    }
}

V4l2BufferPool::~V4l2BufferPool() {
    shutdown();
}

std::optional<DecodedFrameLease> V4l2BufferPool::dequeue() {
    const int device_fd = camera_.fd();
    while (!stop_requested_.load()) {
        const int saved_requeue_error = requeue_error_.load();
        if (saved_requeue_error != 0) {
            throw std::system_error(
                saved_requeue_error,
                std::generic_category(),
                "VIDIOC_QBUF from frame lease");
        }

        pollfd descriptor{};
        descriptor.fd = device_fd;
        descriptor.events = POLLIN | POLLPRI;
        const int poll_result = poll(&descriptor, 1, 250);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw_errno("poll V4L2 capture");
        }
        if (poll_result == 0) {
            continue;
        }
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            throw std::runtime_error(
                "V4L2 capture poll reported a device error");
        }

        v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        if (ioctl(device_fd, VIDIOC_DQBUF, &buffer) < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            }
            throw_errno("VIDIOC_DQBUF");
        }
        if (buffer.index >= mappings_.size()) {
            throw std::runtime_error(
                "VIDIOC_DQBUF returned an invalid buffer index");
        }

        try {
            V4l2BufferMapping& mapping = mappings_[buffer.index];
            const std::size_t valid_size = buffer.bytesused;
            if (valid_size > mapping.length()) {
                throw std::runtime_error(
                    "V4L2 bytesused exceeds mapped buffer length");
            }

            FrameMemoryType memory_type = FrameMemoryType::cpu;
            if (mapping.dma_buf_fd() >= 0) {
                memory_type = FrameMemoryType::dma_buf;
            }

            FrameTimestampSource timestamp_source =
                FrameTimestampSource::dequeue_monotonic;
            std::uint64_t timestamp_ns = MonotonicClock::now_ns();
            if ((buffer.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) ==
                V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC) {
                timestamp_source = FrameTimestampSource::driver_monotonic;
                timestamp_ns = timeval_to_ns(buffer.timestamp);
            }

            std::array<FramePlaneView, FrameView::max_planes> planes{};
            planes[0] = FramePlaneView(
                mapping.data(),
                valid_size,
                bytes_per_line_,
                mapping.dma_buf_fd(),
                0);
            FrameView view(
                to_video_pixel_format(requested_.pixel_format),
                memory_type,
                requested_.width,
                requested_.height,
                buffer.sequence,
                timestamp_ns,
                timestamp_source,
                planes,
                1);

            std::shared_ptr<V4l2BufferPool> owner = shared_from_this();
            const std::uint32_t index = buffer.index;
            return DecodedFrameLease(
                std::move(view),
                RequeueOnLeaseRelease(std::move(owner), index));
        } catch (...) {
            requeue(buffer.index);
            throw;
        }
    }
    return std::nullopt;
}

void V4l2BufferPool::request_stop() noexcept {
    stop_requested_.store(true);
}

void V4l2BufferPool::initialize(
    std::uint32_t requested_buffer_count) {
    const int device_fd = camera_.fd();
    if (device_fd < 0) {
        throw std::invalid_argument(
            "V4L2 buffer pool requires an open device");
    }
    if (requested_buffer_count < 2) {
        throw std::invalid_argument(
            "V4L2 buffer count must be at least two");
    }

    v4l2_requestbuffers request{};
    request.count = requested_buffer_count;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    if (ioctl(device_fd, VIDIOC_REQBUFS, &request) < 0) {
        throw_errno("VIDIOC_REQBUFS");
    }
    if (request.count != requested_buffer_count) {
        throw std::runtime_error(
            "V4L2 driver allocated " +
            std::to_string(request.count) + " buffers, but " +
            std::to_string(requested_buffer_count) +
            " were requested");
    }

    mappings_.reserve(request.count);
    for (std::uint32_t index = 0; index < request.count; ++index) {
        v4l2_buffer query{};
        query.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        query.memory = V4L2_MEMORY_MMAP;
        query.index = index;
        if (ioctl(device_fd, VIDIOC_QUERYBUF, &query) < 0) {
            throw_errno("VIDIOC_QUERYBUF");
        }

        void* address = mmap(
            nullptr,
            query.length,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            device_fd,
            query.m.offset);
        if (address == MAP_FAILED) {
            throw_errno("mmap V4L2 buffer");
        }

        int dma_buf_fd = -1;
        v4l2_exportbuffer export_buffer{};
        export_buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        export_buffer.index = index;
        export_buffer.flags = O_CLOEXEC | O_RDWR;
        if (ioctl(device_fd, VIDIOC_EXPBUF, &export_buffer) == 0) {
            dma_buf_fd = export_buffer.fd;
        }
        mappings_.emplace_back(address, query.length, dma_buf_fd);

        v4l2_buffer queue{};
        queue.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        queue.memory = V4L2_MEMORY_MMAP;
        queue.index = index;
        if (ioctl(device_fd, VIDIOC_QBUF, &queue) < 0) {
            throw_errno("initial VIDIOC_QBUF");
        }
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(device_fd, VIDIOC_STREAMON, &type) < 0) {
        throw_errno("VIDIOC_STREAMON");
    }
    streaming_ = true;
}

void V4l2BufferPool::requeue(std::uint32_t index) noexcept {
    std::lock_guard<std::mutex> lock(requeue_mutex_);
    const int device_fd = camera_.fd();
    if (device_fd < 0 || !streaming_) {
        return;
    }

    v4l2_buffer buffer{};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = index;
    if (ioctl(device_fd, VIDIOC_QBUF, &buffer) < 0) {
        const int error = errno;
        int expected = 0;
        requeue_error_.compare_exchange_strong(expected, error);
    }
}

void V4l2BufferPool::shutdown() noexcept {
    request_stop();
    const int device_fd = camera_.fd();
    if (device_fd >= 0 && streaming_) {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(device_fd, VIDIOC_STREAMOFF, &type);
        streaming_ = false;
    }
    mappings_.clear();
}

}  // namespace vtb
