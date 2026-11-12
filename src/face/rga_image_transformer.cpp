#include "vtb/face/rga_image_transformer.hpp"

#include <array>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>
#include <vector>

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

class ImportedRgaBuffer final {
public:
    ImportedRgaBuffer(
        int dma_buf_fd,
        std::size_t size_bytes,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t horizontal_stride,
        int format)
        : source_fd_(dma_buf_fd),
          size_bytes_(size_bytes),
          width_(width),
          height_(height),
          horizontal_stride_(horizontal_stride),
          format_(format) {
        if (size_bytes > static_cast<std::size_t>(INT_MAX)) {
            throw std::invalid_argument("RGA DMA-BUF is too large to import");
        }
        const int duplicated_fd =
            fcntl(dma_buf_fd, F_DUPFD_CLOEXEC, 0);
        if (duplicated_fd < 0) {
            throw_errno("duplicate RGA source DMA-BUF");
        }
        owned_fd_ = UniqueFd(duplicated_fd);
        handle_ = importbuffer_fd(
            owned_fd_.get(), static_cast<int>(size_bytes));
        if (handle_ == 0) {
            throw std::runtime_error("RGA DMA-BUF import failed");
        }
        buffer_ = wrapbuffer_handle_t(
            handle_,
            static_cast<int>(width),
            static_cast<int>(height),
            static_cast<int>(horizontal_stride),
            static_cast<int>(height),
            format);
    }

    ~ImportedRgaBuffer() {
        if (handle_ != 0) {
            releasebuffer_handle(handle_);
        }
    }

    ImportedRgaBuffer(const ImportedRgaBuffer&) = delete;
    ImportedRgaBuffer& operator=(const ImportedRgaBuffer&) = delete;
    ImportedRgaBuffer(ImportedRgaBuffer&&) = delete;
    ImportedRgaBuffer& operator=(ImportedRgaBuffer&&) = delete;

    rga_buffer_t buffer() const noexcept {
        return buffer_;
    }

    bool matches(
        int dma_buf_fd,
        std::size_t size_bytes,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t horizontal_stride,
        int format) const noexcept {
        return source_fd_ == dma_buf_fd && size_bytes_ == size_bytes &&
            width_ == width && height_ == height &&
            horizontal_stride_ == horizontal_stride && format_ == format;
    }

private:
    int source_fd_ = -1;
    std::size_t size_bytes_ = 0;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t horizontal_stride_ = 0;
    int format_ = RK_FORMAT_UNKNOWN;
    UniqueFd owned_fd_;
    rga_buffer_handle_t handle_ = 0;
    rga_buffer_t buffer_{};
};

class RgaModelInputBuffer final {
public:
    RgaModelInputBuffer(
        const std::string& dma_heap_path,
        std::uint32_t width,
        std::uint32_t height,
        std::size_t minimum_size_bytes)
        : width_(width),
          height_(height),
          stride_bytes_(width * 3U),
          size_bytes_(
              static_cast<std::size_t>(stride_bytes_) *
              static_cast<std::size_t>(height)) {
        if (minimum_size_bytes > size_bytes_) {
            size_bytes_ = minimum_size_bytes;
        }
        UniqueFd heap(open(dma_heap_path.c_str(), O_RDONLY | O_CLOEXEC));
        if (heap.get() < 0) {
            throw_errno("open RGA DMA heap " + dma_heap_path);
        }

        dma_heap_allocation_data allocation{};
        allocation.len = size_bytes_;
        allocation.fd_flags = O_RDWR | O_CLOEXEC;
        if (ioctl(heap.get(), DMA_HEAP_IOCTL_ALLOC, &allocation) < 0) {
            throw_errno("DMA_HEAP_IOCTL_ALLOC for RGA model input");
        }
        fd_ = UniqueFd(static_cast<int>(allocation.fd));
        try {
            mapping_ = mmap(
                nullptr,
                size_bytes_,
                PROT_READ | PROT_WRITE,
                MAP_SHARED,
                fd_.get(),
                0);
            if (mapping_ == MAP_FAILED) {
                throw_errno("mmap RGA model input");
            }

            handle_ = importbuffer_fd(
                fd_.get(), static_cast<int>(size_bytes_));
            if (handle_ == 0) {
                throw std::runtime_error(
                    "RGA model input DMA-BUF import failed");
            }
            buffer_ = wrapbuffer_handle_t(
                handle_,
                static_cast<int>(width_),
                static_cast<int>(height_),
                static_cast<int>(width_),
                static_cast<int>(height_),
                RK_FORMAT_RGB_888);
            imsetColorSpace(&buffer_, IM_RGB_FULL_RANGE);
        } catch (...) {
            reset_mapping_and_handle();
            throw;
        }
    }

    ~RgaModelInputBuffer() {
        reset_mapping_and_handle();
    }

    RgaModelInputBuffer(const RgaModelInputBuffer&) = delete;
    RgaModelInputBuffer& operator=(const RgaModelInputBuffer&) = delete;
    RgaModelInputBuffer(RgaModelInputBuffer&&) = delete;
    RgaModelInputBuffer& operator=(RgaModelInputBuffer&&) = delete;

    std::uint32_t width() const noexcept {
        return width_;
    }

    std::uint32_t height() const noexcept {
        return height_;
    }

    const std::byte* data() const noexcept {
        return static_cast<const std::byte*>(mapping_);
    }

    std::size_t size_bytes() const noexcept {
        return size_bytes_;
    }

    std::uint32_t stride_bytes() const noexcept {
        return stride_bytes_;
    }

    int fd() const noexcept {
        return fd_.get();
    }

    rga_buffer_t buffer() const noexcept {
        return buffer_;
    }

private:
    void reset_mapping_and_handle() noexcept {
        if (handle_ != 0) {
            releasebuffer_handle(handle_);
            handle_ = 0;
        }
        if (mapping_ != MAP_FAILED) {
            munmap(mapping_, size_bytes_);
            mapping_ = MAP_FAILED;
        }
    }

    UniqueFd fd_;
    void* mapping_ = MAP_FAILED;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t stride_bytes_ = 0;
    std::size_t size_bytes_ = 0;
    rga_buffer_handle_t handle_ = 0;
    rga_buffer_t buffer_{};
};

class KeepRgaBufferAlive final {
public:
    explicit KeepRgaBufferAlive(std::shared_ptr<RgaModelInputBuffer> buffer)
        : buffer_(std::move(buffer)) {}
    ~KeepRgaBufferAlive() = default;

    KeepRgaBufferAlive(const KeepRgaBufferAlive&) = default;
    KeepRgaBufferAlive& operator=(const KeepRgaBufferAlive&) = default;
    KeepRgaBufferAlive(KeepRgaBufferAlive&&) noexcept = default;
    KeepRgaBufferAlive& operator=(KeepRgaBufferAlive&&) noexcept = default;

    void operator()() const noexcept {}

private:
    std::shared_ptr<RgaModelInputBuffer> buffer_;
};

class SourceLayout final {
public:
    SourceLayout(int format, std::uint32_t horizontal_stride)
        : format_(format), horizontal_stride_(horizontal_stride) {}
    ~SourceLayout() = default;

    SourceLayout(const SourceLayout&) = default;
    SourceLayout& operator=(const SourceLayout&) = default;
    SourceLayout(SourceLayout&&) noexcept = default;
    SourceLayout& operator=(SourceLayout&&) noexcept = default;

    int format() const noexcept {
        return format_;
    }

    std::uint32_t horizontal_stride() const noexcept {
        return horizontal_stride_;
    }

private:
    int format_ = RK_FORMAT_UNKNOWN;
    std::uint32_t horizontal_stride_ = 0;
};

SourceLayout source_layout(const FrameView& frame) {
    const std::uint32_t stride_bytes = frame.plane(0).stride_bytes();
    if (frame.pixel_format() == VideoPixelFormat::yuyv) {
        if ((stride_bytes & 1U) != 0U) {
            throw std::invalid_argument("YUYV byte stride must be even for RGA");
        }
        return SourceLayout(RK_FORMAT_YUYV_422, stride_bytes / 2U);
    }
    if (frame.pixel_format() == VideoPixelFormat::nv12) {
        return SourceLayout(RK_FORMAT_YCbCr_420_SP, stride_bytes);
    }
    if (frame.pixel_format() == VideoPixelFormat::nv21) {
        return SourceLayout(RK_FORMAT_YCrCb_420_SP, stride_bytes);
    }
    throw std::invalid_argument(
        "RgaImageTransformer supports YUYV, NV12 and NV21 source frames");
}

std::size_t required_source_size(const FrameView& frame) {
    const std::size_t stride = frame.plane(0).stride_bytes();
    const std::size_t height = frame.height();
    if (frame.pixel_format() == VideoPixelFormat::yuyv) {
        return stride * height;
    }
    return stride * height + stride * (height / 2U);
}

std::uint32_t round_down_even(std::uint32_t value) noexcept {
    return value & ~1U;
}

std::uint32_t round_up_even(std::uint32_t value) noexcept {
    return (value + 1U) & ~1U;
}

CropRegion rga_crop(const FrameView& frame, const CropRegion& requested) {
    std::uint32_t left = round_down_even(requested.x());
    std::uint32_t top = round_down_even(requested.y());
    std::uint32_t right = round_up_even(requested.x() + requested.width());
    std::uint32_t bottom =
        round_up_even(requested.y() + requested.height());
    if (right > frame.width()) {
        right = round_down_even(frame.width());
    }
    if (bottom > frame.height()) {
        bottom = round_down_even(frame.height());
    }
    if (right <= left || bottom <= top) {
        throw std::invalid_argument("RGA source crop is empty after YUV alignment");
    }
    return CropRegion(left, top, right - left, bottom - top);
}

void validate_request(
    const FrameView& frame,
    const CropRegion& crop,
    const ModelInputSpec& spec) {
    if (spec.pixel_format() != VideoPixelFormat::rgb888 ||
        spec.layout() != ModelInputLayout::nhwc ||
        spec.data_type() != ModelInputDataType::uint8) {
        throw std::invalid_argument(
            "RgaImageTransformer produces RGB888 UINT8 NHWC only");
    }
    if (frame.plane_count() != 1) {
        throw std::invalid_argument(
            "RgaImageTransformer requires one contiguous source plane");
    }
    if (frame.pixel_format() != VideoPixelFormat::yuyv &&
        frame.pixel_format() != VideoPixelFormat::nv12 &&
        frame.pixel_format() != VideoPixelFormat::nv21) {
        throw std::invalid_argument(
            "RgaImageTransformer supports YUYV, NV12 and NV21 source frames");
    }
    const FramePlaneView& plane = frame.plane(0);
    if (frame.memory_type() != FrameMemoryType::dma_buf ||
        plane.dma_buf_fd() < 0) {
        throw std::invalid_argument(
            "RgaImageTransformer requires a DMA-BUF source frame");
    }
    if (plane.offset_bytes() != 0) {
        throw std::invalid_argument(
            "RgaImageTransformer requires a zero-offset DMA-BUF plane");
    }
    if (crop.x() + crop.width() > frame.width() ||
        crop.y() + crop.height() > frame.height()) {
        throw std::invalid_argument("RGA crop exceeds the source frame");
    }
    if ((frame.width() & 1U) != 0U ||
        ((frame.pixel_format() == VideoPixelFormat::nv12 ||
          frame.pixel_format() == VideoPixelFormat::nv21) &&
         (frame.height() & 1U) != 0U)) {
        throw std::invalid_argument(
            "RGA YUV source dimensions do not meet chroma alignment");
    }
    if (plane.size_bytes() < required_source_size(frame)) {
        throw std::invalid_argument("RGA source DMA-BUF is incomplete");
    }
    const SourceLayout layout = source_layout(frame);
    if (layout.horizontal_stride() < frame.width()) {
        throw std::invalid_argument("RGA source stride is smaller than its width");
    }
}

}  // namespace

class RgaImageTransformer::Impl final {
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

    void begin_source(VideoStreamGeneration generation) {
        if (active_generation_) {
            if (*active_generation_ == generation) {
                return;
            }
            throw std::logic_error(
                "RGA image transformer source generation is still active");
        }
        active_generation_ = generation;
    }

    ModelInputLease prepare(
        const FrameView& frame,
        const CropRegion& crop,
        const ModelInputSpec& spec) {
        validate_request(frame, crop, spec);
        const SourceLayout layout = source_layout(frame);
        const CropRegion effective_crop = rga_crop(frame, crop);
        std::shared_ptr<RgaModelInputBuffer> output = output_buffer(spec);

        const FramePlaneView& source_plane = frame.plane(0);
        rga_buffer_t source_buffer = source_buffer_for(
            frame, source_plane, layout);
        imsetColorSpace(&source_buffer, IM_YUV_BT601_LIMIT_RANGE);
        rga_buffer_t destination_buffer = output->buffer();

        im_rect source_rect{};
        source_rect.x = static_cast<int>(effective_crop.x());
        source_rect.y = static_cast<int>(effective_crop.y());
        source_rect.width = static_cast<int>(effective_crop.width());
        source_rect.height = static_cast<int>(effective_crop.height());
        im_rect destination_rect{};
        destination_rect.width = static_cast<int>(spec.width());
        destination_rect.height = static_cast<int>(spec.height());
        rga_buffer_t pattern_buffer{};
        im_rect pattern_rect{};

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

        std::array<FramePlaneView, FrameView::max_planes> planes{};
        planes[0] = FramePlaneView(
            output->data(),
            output->size_bytes(),
            output->stride_bytes(),
            output->fd(),
            0);
        FrameView transformed(
            VideoPixelFormat::rgb888,
            FrameMemoryType::dma_buf,
            spec.width(),
            spec.height(),
            frame.frame_id(),
            frame.timestamp_ns(),
            frame.timestamp_source(),
            planes,
            1);
        ModelInputView view(
            std::move(transformed), spec, effective_crop);
        return ModelInputLease(
            std::move(view), KeepRgaBufferAlive(std::move(output)));
    }

    void retire_source(VideoStreamGeneration generation) noexcept {
        if (!active_generation_) {
            return;
        }
        if (*active_generation_ != generation) {
            return;
        }
        source_buffers_.clear();
        active_generation_.reset();
    }

private:
    rga_buffer_t source_buffer_for(
        const FrameView& frame,
        const FramePlaneView& plane,
        const SourceLayout& layout) {
        if (!active_generation_) {
            throw std::logic_error(
                "RGA image transformer source generation is not active");
        }
        for (const std::unique_ptr<ImportedRgaBuffer>& buffer :
             source_buffers_) {
            if (buffer->matches(
                    plane.dma_buf_fd(),
                    plane.size_bytes(),
                    frame.width(),
                    frame.height(),
                    layout.horizontal_stride(),
                    layout.format())) {
                return buffer->buffer();
            }
        }
        auto imported = std::make_unique<ImportedRgaBuffer>(
            plane.dma_buf_fd(),
            plane.size_bytes(),
            frame.width(),
            frame.height(),
            layout.horizontal_stride(),
            layout.format());
        rga_buffer_t result = imported->buffer();
        source_buffers_.push_back(std::move(imported));
        return result;
    }

    std::shared_ptr<RgaModelInputBuffer> output_buffer(
        const ModelInputSpec& spec) {
        if (!output_buffer_ ||
            output_buffer_->width() != spec.width() ||
            output_buffer_->height() != spec.height() ||
            output_buffer_->size_bytes() <
                spec.minimum_buffer_size_bytes() ||
            output_buffer_.use_count() != 1) {
            output_buffer_ = std::make_shared<RgaModelInputBuffer>(
                dma_heap_path_, spec.width(), spec.height(),
                spec.minimum_buffer_size_bytes());
        }
        return output_buffer_;
    }

    std::string dma_heap_path_;
    std::optional<VideoStreamGeneration> active_generation_;
    std::vector<std::unique_ptr<ImportedRgaBuffer>> source_buffers_;
    std::shared_ptr<RgaModelInputBuffer> output_buffer_;
};

RgaImageTransformer::RgaImageTransformer(std::string dma_heap_path)
    : impl_(std::make_unique<Impl>(std::move(dma_heap_path))) {}

RgaImageTransformer::~RgaImageTransformer() = default;

void RgaImageTransformer::begin_source(VideoStreamGeneration generation) {
    impl_->begin_source(generation);
}

ModelInputLease RgaImageTransformer::prepare(
    const FrameView& frame,
    const CropRegion& crop,
    const ModelInputSpec& spec) {
    return impl_->prepare(frame, crop, spec);
}

void RgaImageTransformer::retire_source(
    VideoStreamGeneration generation) noexcept {
    impl_->retire_source(generation);
}

}  // namespace vtb
