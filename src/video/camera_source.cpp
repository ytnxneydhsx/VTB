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

/**
 * @brief 管理 V4L2 MMAP 采集缓冲区及每一帧从取出到归还的生命周期。
 *
 * 构造时复制已经完成配置的摄像头文件描述符，向驱动申请多个缓冲区，执行 mmap，
 * 尝试导出 DMA-BUF fd，并把全部缓冲区排入采集队列。dequeue() 取出一帧后返回
 * DecodedFrameLease；租约释放时通过回调执行 requeue()，把同一个缓冲区归还驱动。
 *
 * enable_shared_from_this 允许帧租约持有当前缓冲池的 shared_ptr。即使外部
 * CameraSource 已经关闭，只要还有帧租约，缓冲池及其 mmap 内存就会继续存活。
 * 本类不创建线程；互斥量和原子变量用于协调外部采集、推理和停止线程。
 */
class V4l2BufferPool final
    : public std::enable_shared_from_this<V4l2BufferPool> {
private:
    /**
     * @brief 帧租约释放时把指定缓冲区归还给所属缓冲池。
     *
     * owner_ 延长缓冲池生命周期，index_ 标识需要重新执行 VIDIOC_QBUF 的槽位。
     * DecodedFrameLease 通过 std::function 调用 operator()()。
     */
    class RequeueOnLeaseRelease final {
    public:
        RequeueOnLeaseRelease(
            std::shared_ptr<V4l2BufferPool> owner,
            std::uint32_t index)
            : owner_(std::move(owner)), index_(index) {}

        void operator()() const noexcept;

    private:
        /** @brief 持有缓冲池，保证帧租约有效期间 mmap 内存不会释放。 */
        std::shared_ptr<V4l2BufferPool> owner_;

        /** @brief 当前帧对应的 V4L2 缓冲区编号。 */
        std::uint32_t index_ = 0;
    };

public:
    /**
     * @brief 接管采集所需状态并启动 V4L2 streaming。
     * @param configured_fd 已完成格式和帧率配置的摄像头文件描述符。
     * @param requested JSON 中的请求配置，用于描述输出帧格式和尺寸。
     * @param bytes_per_line 驱动返回的行跨度，包含可能存在的行对齐填充。
     * @param requested_buffer_count 向驱动申请的缓冲区数量，至少为两个；驱动必须
     * 返回完全相同的数量。
     *
     * configured_fd 会通过 F_DUPFD_CLOEXEC 复制，缓冲池独立关闭复制后的 fd。
     */
    V4l2BufferPool(
        int configured_fd,
        CaptureConfig requested,
        std::uint32_t bytes_per_line,
        std::uint32_t requested_buffer_count)
        : requested_(std::move(requested)),
          bytes_per_line_(bytes_per_line) {
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

    /** @brief 停止采集，解除映射并关闭缓冲池持有的文件描述符。 */
    ~V4l2BufferPool() {
        shutdown();
    }

    /**
     * 缓冲池独占 fd、mmap 映射和同步状态，因此禁止复制和移动。
     * 帧租约通过 shared_ptr 延长同一个缓冲池对象的生命周期。
     */
    V4l2BufferPool(const V4l2BufferPool&) = delete;
    V4l2BufferPool& operator=(const V4l2BufferPool&) = delete;
    V4l2BufferPool(V4l2BufferPool&&) = delete;
    V4l2BufferPool& operator=(V4l2BufferPool&&) = delete;

    /**
     * @brief 等待并取出下一帧摄像头缓冲区。
     *
     * 函数使用 poll() 等待设备就绪，再通过 VIDIOC_DQBUF 取得缓冲区。返回的帧租约
     * 直接引用 mmap 内存；租约析构时自动调用 requeue()。收到停止请求时返回空值。
     *
     * @return 一帧有效租约；停止采集时返回 std::nullopt。
     * @throws std::system_error poll、DQBUF 或先前的 QBUF 操作失败。
     * @throws std::runtime_error 驱动报告错误或返回越界的缓冲区信息。
     */
    std::optional<DecodedFrameLease> dequeue() {
        while (!stop_requested_.load()) {
            const int saved_requeue_error = requeue_error_.load();
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

    /**
     * @brief 请求 dequeue() 停止等待并尽快返回空值。
     *
     * 这里只设置原子标志。dequeue() 的 poll 超时时间为 250 ms，因此不需要关闭 fd
     * 来打断等待。
     */
    void request_stop() noexcept {
        stop_requested_.store(true);
    }

private:
    /**
     * @brief 申请、映射、导出并排队 V4L2 缓冲区，最后启动视频流。
     * @param requested_buffer_count 请求的 MMAP 缓冲区数量。
     */
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

    /**
     * @brief 把帧租约使用完毕的缓冲区重新交给 V4L2 驱动。
     * @param index 需要执行 VIDIOC_QBUF 的驱动缓冲区编号。
     *
     * 本函数用于租约析构回调，因此保持 noexcept。QBUF 失败时保存第一个 errno，
     * 下一次 dequeue() 会在正常异常边界中报告该错误。
     */
    void requeue(std::uint32_t index) noexcept {
        std::lock_guard<std::mutex> lock(requeue_mutex_);
        if (fd_ < 0 || !streaming_) {
            return;
        }

        v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        if (ioctl(fd_, VIDIOC_QBUF, &buffer) < 0) {
            const int error = errno;
            int expected = 0;
            requeue_error_.compare_exchange_strong(expected, error);
        }
    }

    /**
     * @brief 以可重复调用的方式停止采集并释放全部系统资源。
     *
     * 释放顺序为停止请求、STREAMOFF、解除所有 mmap、关闭 fd。
     */
    void shutdown() noexcept {
        request_stop();
        if (fd_ >= 0 && streaming_) {
            v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            ioctl(fd_, VIDIOC_STREAMOFF, &type);
            streaming_ = false;
        }
        mappings_.clear();
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
    }

    /** @brief 缓冲池独占的摄像头文件描述符，由 configured_fd 复制得到。 */
    int fd_ = -1;

    /** @brief JSON 请求配置，用来构造 FrameView 的格式、宽度和高度。 */
    CaptureConfig requested_;

    /** @brief 相邻两行图像起始地址之间的字节距离。 */
    std::uint32_t bytes_per_line_ = 0;

    /** @brief 每个驱动缓冲区对应的 mmap 地址、长度和可选 DMA-BUF fd。 */
    std::vector<BufferMapping> mappings_;

    /** @brief 串行化可能由不同线程同时触发的 VIDIOC_QBUF 操作。 */
    std::mutex requeue_mutex_;

    /** @brief 外部发出的停止请求，由 dequeue() 循环读取。 */
    std::atomic<bool> stop_requested_{false};

    /** @brief requeue() 首次失败时保存的 errno，零表示没有错误。 */
    std::atomic<int> requeue_error_{0};

    /** @brief STREAMON 已成功且尚未执行 STREAMOFF 时为 true。 */
    bool streaming_ = false;
};

void V4l2BufferPool::RequeueOnLeaseRelease::operator()() const noexcept {
    owner_->requeue(index_);
}

CameraSource::CameraSource(CaptureConfig config, std::uint32_t buffer_count) {
    V4l2Camera camera(config);
    const std::uint32_t bytes_per_line = camera.open_and_configure();
    to_video_pixel_format(config.pixel_format);
    buffer_pool_ = std::make_shared<V4l2BufferPool>(
        camera.fd(),
        std::move(config),
        bytes_per_line,
        buffer_count);
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

}  // namespace vtb
