#include "vtb/hardware/rga/rga_device.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <fcntl.h>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>

#include <linux/dma-heap.h>
#include <rga/im2d.h>

namespace vtb {
namespace {

[[noreturn]] void throw_errno(const std::string& operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

void require_rga_success(IM_STATUS status, const std::string& operation) {
    if (status != IM_STATUS_SUCCESS && status != IM_STATUS_NOERROR) {
        throw std::runtime_error(
            operation + " failed: " + std::string(imStrError_t(status)));
    }
}

int native_format(RgaPixelFormat format) {
    if (format == RgaPixelFormat::yuyv422) {
        return RK_FORMAT_YUYV_422;
    }
    if (format == RgaPixelFormat::nv12) {
        return RK_FORMAT_YCbCr_420_SP;
    }
    if (format == RgaPixelFormat::nv21) {
        return RK_FORMAT_YCrCb_420_SP;
    }
    if (format == RgaPixelFormat::rgb888) {
        return RK_FORMAT_RGB_888;
    }
    throw std::invalid_argument("Unsupported RGA pixel format");
}

class UniqueFd final {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    ~UniqueFd() {
        reset();
    }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    int get() const noexcept {
        return fd_;
    }

private:
    int release() noexcept {
        const int result = fd_;
        fd_ = -1;
        return result;
    }

    void reset(int replacement = -1) noexcept {
        if (fd_ >= 0) {
            close(fd_);
        }
        fd_ = replacement;
    }

    int fd_ = -1;
};

}  // namespace

class RgaBuffer::Impl final {
public:
    Impl(
        UniqueFd fd,
        void* mapping,
        bool mapping_owned,
        std::size_t size_bytes,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t horizontal_stride,
        std::uint32_t vertical_stride,
        std::uint32_t stride_bytes,
        RgaPixelFormat format)
        : fd_(std::move(fd)),
          mapping_(mapping),
          mapping_owned_(mapping_owned),
          size_bytes_(size_bytes),
          width_(width),
          height_(height),
          horizontal_stride_(horizontal_stride),
          vertical_stride_(vertical_stride),
          stride_bytes_(stride_bytes),
          format_(format) {
        if (size_bytes_ > static_cast<std::size_t>(INT_MAX)) {
            throw std::invalid_argument("RGA DMA-BUF is too large to import");
        }
        handle_ = importbuffer_fd(fd_.get(), static_cast<int>(size_bytes_));
        if (handle_ == 0) {
            throw std::runtime_error("RGA DMA-BUF import failed");
        }
        buffer_ = wrapbuffer_handle_t(
            handle_,
            static_cast<int>(width_),
            static_cast<int>(height_),
            static_cast<int>(horizontal_stride_),
            static_cast<int>(vertical_stride_),
            native_format(format_));
    }

    ~Impl() {
        if (handle_ != 0) {
            releasebuffer_handle(handle_);
        }
        if (mapping_owned_ && mapping_ != MAP_FAILED) {
            munmap(mapping_, size_bytes_);
        }
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    rga_buffer_t buffer() const noexcept {
        return buffer_;
    }

    const std::byte* data() const noexcept {
        if (mapping_ == MAP_FAILED) {
            return nullptr;
        }
        return static_cast<const std::byte*>(mapping_);
    }

    int fd() const noexcept { return fd_.get(); }
    std::size_t size_bytes() const noexcept { return size_bytes_; }
    std::uint32_t width() const noexcept { return width_; }
    std::uint32_t height() const noexcept { return height_; }
    std::uint32_t stride_bytes() const noexcept { return stride_bytes_; }
    RgaPixelFormat format() const noexcept { return format_; }

private:
    UniqueFd fd_;
    void* mapping_ = MAP_FAILED;
    bool mapping_owned_ = false;
    std::size_t size_bytes_ = 0;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t horizontal_stride_ = 0;
    std::uint32_t vertical_stride_ = 0;
    std::uint32_t stride_bytes_ = 0;
    RgaPixelFormat format_ = RgaPixelFormat::rgb888;
    rga_buffer_handle_t handle_ = 0;
    rga_buffer_t buffer_{};
};

class RgaDevice::Impl final {
public:
    explicit Impl(std::string dma_heap_path)
        : dma_heap_path_(std::move(dma_heap_path)) {
        if (dma_heap_path_.empty()) {
            throw std::invalid_argument("RGA DMA heap path must not be empty");
        }
    }
    ~Impl() = default;

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    const std::string& dma_heap_path() const noexcept {
        return dma_heap_path_;
    }

    std::mutex& submission_mutex() noexcept {
        return submission_mutex_;
    }

private:
    std::string dma_heap_path_;
    std::mutex submission_mutex_;
};

RgaBufferDescription::RgaBufferDescription(
    int dma_buf_fd,
    std::size_t size_bytes,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t horizontal_stride,
    std::uint32_t vertical_stride,
    std::uint32_t stride_bytes,
    RgaPixelFormat format)
    : dma_buf_fd_(dma_buf_fd),
      size_bytes_(size_bytes),
      width_(width),
      height_(height),
      horizontal_stride_(horizontal_stride),
      vertical_stride_(vertical_stride),
      stride_bytes_(stride_bytes),
      format_(format) {}

int RgaBufferDescription::dma_buf_fd() const noexcept { return dma_buf_fd_; }
std::size_t RgaBufferDescription::size_bytes() const noexcept { return size_bytes_; }
std::uint32_t RgaBufferDescription::width() const noexcept { return width_; }
std::uint32_t RgaBufferDescription::height() const noexcept { return height_; }
std::uint32_t RgaBufferDescription::horizontal_stride() const noexcept { return horizontal_stride_; }
std::uint32_t RgaBufferDescription::vertical_stride() const noexcept { return vertical_stride_; }
std::uint32_t RgaBufferDescription::stride_bytes() const noexcept { return stride_bytes_; }
RgaPixelFormat RgaBufferDescription::format() const noexcept { return format_; }

RgaRectangle::RgaRectangle(
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t width,
    std::uint32_t height) noexcept
    : x_(x), y_(y), width_(width), height_(height) {}

std::uint32_t RgaRectangle::x() const noexcept { return x_; }
std::uint32_t RgaRectangle::y() const noexcept { return y_; }
std::uint32_t RgaRectangle::width() const noexcept { return width_; }
std::uint32_t RgaRectangle::height() const noexcept { return height_; }

RgaBuffer::RgaBuffer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
RgaBuffer::~RgaBuffer() = default;
const std::byte* RgaBuffer::data() const noexcept { return impl_->data(); }
int RgaBuffer::dma_buf_fd() const noexcept { return impl_->fd(); }
std::size_t RgaBuffer::size_bytes() const noexcept { return impl_->size_bytes(); }
std::uint32_t RgaBuffer::width() const noexcept { return impl_->width(); }
std::uint32_t RgaBuffer::height() const noexcept { return impl_->height(); }
std::uint32_t RgaBuffer::stride_bytes() const noexcept { return impl_->stride_bytes(); }
RgaPixelFormat RgaBuffer::format() const noexcept { return impl_->format(); }

RgaDevice::RgaDevice(std::string dma_heap_path)
    : impl_(std::make_unique<Impl>(std::move(dma_heap_path))) {}
RgaDevice::~RgaDevice() = default;

std::shared_ptr<RgaBuffer> RgaDevice::register_dma_buf(
    const RgaBufferDescription& description) {
    if (description.dma_buf_fd() < 0 || description.size_bytes() == 0) {
        throw std::invalid_argument("RGA DMA-BUF registration is invalid");
    }
    const int duplicated_fd =
        fcntl(description.dma_buf_fd(), F_DUPFD_CLOEXEC, 0);
    if (duplicated_fd < 0) {
        throw_errno("duplicate RGA source DMA-BUF");
    }
    auto buffer_impl = std::make_unique<RgaBuffer::Impl>(
        UniqueFd(duplicated_fd),
        MAP_FAILED,
        false,
        description.size_bytes(),
        description.width(),
        description.height(),
        description.horizontal_stride(),
        description.vertical_stride(),
        description.stride_bytes(),
        description.format());
    return std::shared_ptr<RgaBuffer>(
        new RgaBuffer(std::move(buffer_impl)));
}

std::shared_ptr<RgaBuffer> RgaDevice::allocate_buffer(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t horizontal_stride,
    std::uint32_t vertical_stride,
    std::uint32_t stride_bytes,
    std::size_t minimum_size_bytes,
    RgaPixelFormat format) {
    std::size_t size_bytes = static_cast<std::size_t>(stride_bytes) *
        static_cast<std::size_t>(vertical_stride);
    size_bytes = std::max(size_bytes, minimum_size_bytes);
    if (size_bytes == 0 || size_bytes > static_cast<std::size_t>(INT_MAX)) {
        throw std::invalid_argument("RGA output buffer size is invalid");
    }

    UniqueFd heap(open(
        impl_->dma_heap_path().c_str(), O_RDONLY | O_CLOEXEC));
    if (heap.get() < 0) {
        throw_errno("open RGA DMA heap " + impl_->dma_heap_path());
    }
    dma_heap_allocation_data allocation{};
    allocation.len = size_bytes;
    allocation.fd_flags = O_RDWR | O_CLOEXEC;
    if (ioctl(heap.get(), DMA_HEAP_IOCTL_ALLOC, &allocation) < 0) {
        throw_errno("DMA_HEAP_IOCTL_ALLOC for RGA output");
    }
    UniqueFd output_fd(static_cast<int>(allocation.fd));
    void* mapping = mmap(
        nullptr,
        size_bytes,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        output_fd.get(),
        0);
    if (mapping == MAP_FAILED) {
        throw_errno("mmap RGA output");
    }
    std::unique_ptr<RgaBuffer::Impl> buffer_impl;
    try {
        buffer_impl = std::make_unique<RgaBuffer::Impl>(
            std::move(output_fd),
            mapping,
            true,
            size_bytes,
            width,
            height,
            horizontal_stride,
            vertical_stride,
            stride_bytes,
            format);
    } catch (...) {
        munmap(mapping, size_bytes);
        throw;
    }
    return std::shared_ptr<RgaBuffer>(
        new RgaBuffer(std::move(buffer_impl)));
}

void RgaDevice::crop_scale_convert(
    const RgaBuffer& source,
    const RgaRectangle& source_rectangle,
    const RgaBuffer& destination) {
    rga_buffer_t source_buffer = source.impl_->buffer();
    rga_buffer_t destination_buffer = destination.impl_->buffer();
    if (source.format() == RgaPixelFormat::yuyv422 ||
        source.format() == RgaPixelFormat::nv12 ||
        source.format() == RgaPixelFormat::nv21) {
        imsetColorSpace(&source_buffer, IM_YUV_BT601_LIMIT_RANGE);
    }
    if (destination.format() == RgaPixelFormat::rgb888) {
        imsetColorSpace(&destination_buffer, IM_RGB_FULL_RANGE);
    }

    im_rect source_rect{};
    source_rect.x = static_cast<int>(source_rectangle.x());
    source_rect.y = static_cast<int>(source_rectangle.y());
    source_rect.width = static_cast<int>(source_rectangle.width());
    source_rect.height = static_cast<int>(source_rectangle.height());
    im_rect destination_rect{};
    destination_rect.width = static_cast<int>(destination.width());
    destination_rect.height = static_cast<int>(destination.height());
    rga_buffer_t pattern_buffer{};
    im_rect pattern_rect{};

    std::lock_guard<std::mutex> lock(impl_->submission_mutex());
    require_rga_success(
        imcheck_t(
            source_buffer,
            destination_buffer,
            pattern_buffer,
            source_rect,
            destination_rect,
            pattern_rect,
            0),
        "RGA crop/scale/color-conversion check");
    require_rga_success(
        improcess(
            source_buffer,
            destination_buffer,
            pattern_buffer,
            source_rect,
            destination_rect,
            pattern_rect,
            IM_SYNC),
        "RGA crop/scale/color conversion");
}

}  // namespace vtb
