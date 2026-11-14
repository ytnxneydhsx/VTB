#include "vtb/hardware/rga/rga_image_transformer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace vtb {
namespace {

class KeepRgaBufferAlive final {
public:
    explicit KeepRgaBufferAlive(std::shared_ptr<RgaBuffer> buffer)
        : buffer_(std::move(buffer)) {}
    ~KeepRgaBufferAlive() = default;

    KeepRgaBufferAlive(const KeepRgaBufferAlive&) = default;
    KeepRgaBufferAlive& operator=(const KeepRgaBufferAlive&) = default;
    KeepRgaBufferAlive(KeepRgaBufferAlive&&) noexcept = default;
    KeepRgaBufferAlive& operator=(KeepRgaBufferAlive&&) noexcept = default;

    void operator()() const noexcept {}

private:
    std::shared_ptr<RgaBuffer> buffer_;
};

class SourceLayout final {
public:
    SourceLayout(
        RgaPixelFormat format,
        std::uint32_t horizontal_stride) noexcept
        : format_(format), horizontal_stride_(horizontal_stride) {}
    ~SourceLayout() = default;

    SourceLayout(const SourceLayout&) = default;
    SourceLayout& operator=(const SourceLayout&) = default;
    SourceLayout(SourceLayout&&) noexcept = default;
    SourceLayout& operator=(SourceLayout&&) noexcept = default;

    RgaPixelFormat format() const noexcept { return format_; }
    std::uint32_t horizontal_stride() const noexcept {
        return horizontal_stride_;
    }

private:
    RgaPixelFormat format_ = RgaPixelFormat::rgb888;
    std::uint32_t horizontal_stride_ = 0;
};

class SourceRegistration final {
public:
    SourceRegistration(
        int source_fd,
        std::size_t size_bytes,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t horizontal_stride,
        RgaPixelFormat format,
        std::shared_ptr<RgaBuffer> buffer)
        : source_fd_(source_fd),
          size_bytes_(size_bytes),
          width_(width),
          height_(height),
          horizontal_stride_(horizontal_stride),
          format_(format),
          buffer_(std::move(buffer)) {}
    ~SourceRegistration() = default;

    SourceRegistration(const SourceRegistration&) = default;
    SourceRegistration& operator=(const SourceRegistration&) = default;
    SourceRegistration(SourceRegistration&&) noexcept = default;
    SourceRegistration& operator=(SourceRegistration&&) noexcept = default;

    bool matches(
        int source_fd,
        std::size_t size_bytes,
        std::uint32_t width,
        std::uint32_t height,
        const SourceLayout& layout) const noexcept {
        return source_fd_ == source_fd && size_bytes_ == size_bytes &&
            width_ == width && height_ == height &&
            horizontal_stride_ == layout.horizontal_stride() &&
            format_ == layout.format();
    }

    const std::shared_ptr<RgaBuffer>& buffer() const noexcept {
        return buffer_;
    }

private:
    int source_fd_ = -1;
    std::size_t size_bytes_ = 0;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t horizontal_stride_ = 0;
    RgaPixelFormat format_ = RgaPixelFormat::rgb888;
    std::shared_ptr<RgaBuffer> buffer_;
};

SourceLayout source_layout(const FrameView& frame) {
    const std::uint32_t stride_bytes = frame.plane(0).stride_bytes();
    if (frame.pixel_format() == VideoPixelFormat::yuyv) {
        if ((stride_bytes & 1U) != 0U) {
            throw std::invalid_argument("YUYV byte stride must be even for RGA");
        }
        return SourceLayout(RgaPixelFormat::yuyv422, stride_bytes / 2U);
    }
    if (frame.pixel_format() == VideoPixelFormat::nv12) {
        return SourceLayout(RgaPixelFormat::nv12, stride_bytes);
    }
    if (frame.pixel_format() == VideoPixelFormat::nv21) {
        return SourceLayout(RgaPixelFormat::nv21, stride_bytes);
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
    const std::uint32_t left = round_down_even(requested.x());
    const std::uint32_t top = round_down_even(requested.y());
    std::uint32_t right = round_up_even(requested.x() + requested.width());
    std::uint32_t bottom = round_up_even(requested.y() + requested.height());
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
    explicit Impl(std::shared_ptr<RgaDevice> device)
        : device_(std::move(device)) {
        if (!device_) {
            throw std::invalid_argument(
                "RGA image transformer device must not be null");
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
        std::shared_ptr<RgaBuffer> output = output_buffer(spec);
        const std::shared_ptr<RgaBuffer> source =
            source_buffer_for(frame, frame.plane(0), layout);

        device_->crop_scale_convert(
            *source,
            RgaRectangle(
                effective_crop.x(),
                effective_crop.y(),
                effective_crop.width(),
                effective_crop.height()),
            *output);

        std::array<FramePlaneView, FrameView::max_planes> planes{};
        planes[0] = FramePlaneView(
            output->data(),
            output->size_bytes(),
            output->stride_bytes(),
            output->dma_buf_fd(),
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
        ModelInputView view(std::move(transformed), spec, effective_crop);
        return ModelInputLease(
            std::move(view), KeepRgaBufferAlive(std::move(output)));
    }

    void retire_source(VideoStreamGeneration generation) noexcept {
        if (!active_generation_ || *active_generation_ != generation) {
            return;
        }
        source_buffers_.clear();
        active_generation_.reset();
    }

private:
    std::shared_ptr<RgaBuffer> source_buffer_for(
        const FrameView& frame,
        const FramePlaneView& plane,
        const SourceLayout& layout) {
        if (!active_generation_) {
            throw std::logic_error(
                "RGA image transformer source generation is not active");
        }
        for (const SourceRegistration& registration : source_buffers_) {
            if (registration.matches(
                    plane.dma_buf_fd(),
                    plane.size_bytes(),
                    frame.width(),
                    frame.height(),
                    layout)) {
                return registration.buffer();
            }
        }

        std::shared_ptr<RgaBuffer> buffer = device_->register_dma_buf(
            RgaBufferDescription(
                plane.dma_buf_fd(),
                plane.size_bytes(),
                frame.width(),
                frame.height(),
                layout.horizontal_stride(),
                frame.height(),
                plane.stride_bytes(),
                layout.format()));
        source_buffers_.emplace_back(
            plane.dma_buf_fd(),
            plane.size_bytes(),
            frame.width(),
            frame.height(),
            layout.horizontal_stride(),
            layout.format(),
            buffer);
        return buffer;
    }

    std::shared_ptr<RgaBuffer> output_buffer(const ModelInputSpec& spec) {
        if (!output_buffer_ ||
            output_buffer_->width() != spec.width() ||
            output_buffer_->height() != spec.height() ||
            output_buffer_->size_bytes() < spec.minimum_buffer_size_bytes() ||
            output_buffer_.use_count() != 1) {
            output_buffer_ = device_->allocate_buffer(
                spec.width(),
                spec.height(),
                spec.width(),
                spec.height(),
                spec.width() * 3U,
                spec.minimum_buffer_size_bytes(),
                RgaPixelFormat::rgb888);
        }
        return output_buffer_;
    }

    std::shared_ptr<RgaDevice> device_;
    std::optional<VideoStreamGeneration> active_generation_;
    std::vector<SourceRegistration> source_buffers_;
    std::shared_ptr<RgaBuffer> output_buffer_;
};

RgaImageTransformer::RgaImageTransformer(
    std::shared_ptr<RgaDevice> device)
    : impl_(std::make_unique<Impl>(std::move(device))) {}

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
