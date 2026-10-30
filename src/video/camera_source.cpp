#include "vtb/video/camera_source.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include <linux/videodev2.h>

#include "vtb/v4l2_camera.hpp"

namespace vtb {
namespace {

[[noreturn]] void throw_errno(const std::string& operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

std::uint64_t monotonic_now_ns() {
    timespec timestamp{};
    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0) {
        throw_errno("clock_gettime(CLOCK_MONOTONIC)");
    }
    return static_cast<std::uint64_t>(timestamp.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(timestamp.tv_nsec);
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

class BufferMapping {
public:
    BufferMapping() = default;
    BufferMapping(void* address, std::size_t length, int dma_buf_fd) noexcept
        : address_(address), length_(length), dma_buf_fd_(dma_buf_fd) {}
    ~BufferMapping() {
        reset();
    }

    BufferMapping(const BufferMapping&) = delete;
    BufferMapping& operator=(const BufferMapping&) = delete;
    BufferMapping(BufferMapping&& other) noexcept
        : address_(other.address_),
          length_(other.length_),
          dma_buf_fd_(other.dma_buf_fd_) {
        other.address_ = MAP_FAILED;
        other.length_ = 0;
        other.dma_buf_fd_ = -1;
    }
    BufferMapping& operator=(BufferMapping&& other) noexcept {
        if (this != &other) {
            reset();
            address_ = other.address_;
            length_ = other.length_;
            dma_buf_fd_ = other.dma_buf_fd_;
            other.address_ = MAP_FAILED;
            other.length_ = 0;
            other.dma_buf_fd_ = -1;
        }
        return *this;
    }

    const std::byte* data() const noexcept {
        return static_cast<const std::byte*>(address_);
    }

    std::size_t length() const noexcept {
        return length_;
    }

    int dma_buf_fd() const noexcept {
        return dma_buf_fd_;
    }

private:
    void reset() noexcept {
        if (address_ != MAP_FAILED) {
            munmap(address_, length_);
        }
        if (dma_buf_fd_ >= 0) {
            close(dma_buf_fd_);
        }
        address_ = MAP_FAILED;
        length_ = 0;
        dma_buf_fd_ = -1;
    }

    void* address_ = MAP_FAILED;
    std::size_t length_ = 0;
    int dma_buf_fd_ = -1;
};

}  // namespace

class V4l2BufferPool final
    : public std::enable_shared_from_this<V4l2BufferPool> {
public:
    V4l2BufferPool(
        int configured_fd,
        NegotiatedCaptureConfig config,
        std::uint32_t requested_buffer_count)
        : config_(std::move(config)) {
        fd_ = fcntl(configured_fd, F_DUPFD_CLOEXEC, 0);
        if (fd_ < 0) {
            throw_errno("duplicate configured V4L2 descriptor");
        }
        try {
            initialize(requested_buffer_count);
        } catch (...) {
            shutdown();
            throw;
        }
    }

    ~V4l2BufferPool() {
        shutdown();
    }

    V4l2BufferPool(const V4l2BufferPool&) = delete;
    V4l2BufferPool& operator=(const V4l2BufferPool&) = delete;
    V4l2BufferPool(V4l2BufferPool&&) = delete;
    V4l2BufferPool& operator=(V4l2BufferPool&&) = delete;

    std::optional<DecodedFrameLease> dequeue() {
        while (!stop_requested_.load(std::memory_order_acquire)) {
            const int saved_requeue_error =
                requeue_error_.load(std::memory_order_acquire);
            if (saved_requeue_error != 0) {
                throw std::system_error(
                    saved_requeue_error,
                    std::generic_category(),
                    "VIDIOC_QBUF from frame lease");
            }

            pollfd descriptor{};
            descriptor.fd = fd_;
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
                throw std::runtime_error("V4L2 capture poll reported a device error");
            }

            v4l2_buffer buffer{};
            buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buffer.memory = V4L2_MEMORY_MMAP;
            if (ioctl(fd_, VIDIOC_DQBUF, &buffer) < 0) {
                if (errno == EAGAIN || errno == EINTR) {
                    continue;
                }
                throw_errno("VIDIOC_DQBUF");
            }
            if (buffer.index >= mappings_.size()) {
                throw std::runtime_error("VIDIOC_DQBUF returned an invalid buffer index");
            }

            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                if (in_use_[buffer.index]) {
                    throw std::logic_error("V4L2 buffer dequeued while still leased");
                }
                in_use_[buffer.index] = true;
            }

            try {
                BufferMapping& mapping = mappings_[buffer.index];
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
                std::uint64_t timestamp_ns = monotonic_now_ns();
                if ((buffer.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) ==
                    V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC) {
                    timestamp_source = FrameTimestampSource::driver_monotonic;
                    timestamp_ns = timeval_to_ns(buffer.timestamp);
                }

                std::array<FramePlaneView, FrameView::max_planes> planes{};
                planes[0] = FramePlaneView(
                    mapping.data(),
                    valid_size,
                    config_.bytes_per_line,
                    mapping.dma_buf_fd(),
                    0);
                FrameView view(
                    to_video_pixel_format(config_.pixel_format),
                    memory_type,
                    config_.width,
                    config_.height,
                    buffer.sequence,
                    timestamp_ns,
                    timestamp_source,
                    planes,
                    1);

                std::shared_ptr<V4l2BufferPool> owner = shared_from_this();
                const std::uint32_t index = buffer.index;
                return DecodedFrameLease(
                    std::move(view),
                    [owner = std::move(owner), index]() {
                        owner->requeue(index);
                    });
            } catch (...) {
                requeue(buffer.index);
                throw;
            }
        }
        return std::nullopt;
    }

    void request_stop() noexcept {
        stop_requested_.store(true, std::memory_order_release);
    }

private:
    void initialize(std::uint32_t requested_buffer_count) {
        if (fd_ < 0) {
            throw std::invalid_argument("V4L2 buffer pool requires an open device");
        }
        if (requested_buffer_count < 2) {
            throw std::invalid_argument("V4L2 buffer count must be at least two");
        }

        v4l2_requestbuffers request{};
        request.count = requested_buffer_count;
        request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        request.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd_, VIDIOC_REQBUFS, &request) < 0) {
            throw_errno("VIDIOC_REQBUFS");
        }
        if (request.count < 2) {
            throw std::runtime_error("V4L2 driver allocated fewer than two buffers");
        }

        mappings_.reserve(request.count);
        in_use_.assign(request.count, false);
        for (std::uint32_t index = 0; index < request.count; ++index) {
            v4l2_buffer query{};
            query.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            query.memory = V4L2_MEMORY_MMAP;
            query.index = index;
            if (ioctl(fd_, VIDIOC_QUERYBUF, &query) < 0) {
                throw_errno("VIDIOC_QUERYBUF");
            }

            void* address = mmap(
                nullptr,
                query.length,
                PROT_READ | PROT_WRITE,
                MAP_SHARED,
                fd_,
                query.m.offset);
            if (address == MAP_FAILED) {
                throw_errno("mmap V4L2 buffer");
            }

            int dma_buf_fd = -1;
            v4l2_exportbuffer export_buffer{};
            export_buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            export_buffer.index = index;
            export_buffer.flags = O_CLOEXEC | O_RDWR;
            if (ioctl(fd_, VIDIOC_EXPBUF, &export_buffer) == 0) {
                dma_buf_fd = export_buffer.fd;
            }
            mappings_.emplace_back(address, query.length, dma_buf_fd);

            v4l2_buffer queue{};
            queue.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            queue.memory = V4L2_MEMORY_MMAP;
            queue.index = index;
            if (ioctl(fd_, VIDIOC_QBUF, &queue) < 0) {
                throw_errno("initial VIDIOC_QBUF");
            }
        }

        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
            throw_errno("VIDIOC_STREAMON");
        }
        streaming_ = true;
    }

    void requeue(std::uint32_t index) noexcept {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        if (index >= in_use_.size() || !in_use_[index]) {
            return;
        }
        if (fd_ >= 0 && streaming_) {
            v4l2_buffer buffer{};
            buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buffer.memory = V4L2_MEMORY_MMAP;
            buffer.index = index;
            if (ioctl(fd_, VIDIOC_QBUF, &buffer) < 0) {
                int expected = 0;
                requeue_error_.compare_exchange_strong(
                    expected,
                    errno,
                    std::memory_order_release,
                    std::memory_order_relaxed);
                return;
            }
        }
        in_use_[index] = false;
    }

    void shutdown() noexcept {
        request_stop();
        if (fd_ >= 0 && streaming_) {
            v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            ioctl(fd_, VIDIOC_STREAMOFF, &type);
            streaming_ = false;
        }
        mappings_.clear();
        in_use_.clear();
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
    }

    int fd_ = -1;
    NegotiatedCaptureConfig config_;
    std::vector<BufferMapping> mappings_;
    std::vector<bool> in_use_;
    std::mutex buffer_mutex_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<int> requeue_error_{0};
    bool streaming_ = false;
};

CameraSource::CameraSource(CaptureConfig config, std::uint32_t buffer_count) {
    V4l2Camera camera(std::move(config));
    negotiated_config_ = camera.open_and_configure();
    to_video_pixel_format(negotiated_config_.pixel_format);
    buffer_pool_ = std::make_shared<V4l2BufferPool>(
        camera.fd(), negotiated_config_, buffer_count);
}

CameraSource::~CameraSource() {
    close();
}

std::optional<DecodedFrameLease> CameraSource::read() {
    if (!buffer_pool_) {
        return std::nullopt;
    }
    return buffer_pool_->dequeue();
}

void CameraSource::request_stop() noexcept {
    if (buffer_pool_) {
        buffer_pool_->request_stop();
    }
}

void CameraSource::close() noexcept {
    request_stop();
    buffer_pool_.reset();
}

const NegotiatedCaptureConfig& CameraSource::negotiated_config() const noexcept {
    return negotiated_config_;
}

}  // namespace vtb
