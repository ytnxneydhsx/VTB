#include "vtb/hardware/rga/rga_model_input_transformer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace vtb {
namespace {

class KeepRgaTransformAlive final {
public:
    explicit KeepRgaTransformAlive(
        std::shared_ptr<RgaTransformLease> lease)
        : lease_(std::move(lease)) {}
    ~KeepRgaTransformAlive() = default;

    KeepRgaTransformAlive(const KeepRgaTransformAlive&) = default;
    KeepRgaTransformAlive& operator=(const KeepRgaTransformAlive&) = default;
    KeepRgaTransformAlive(KeepRgaTransformAlive&&) noexcept = default;
    KeepRgaTransformAlive& operator=(KeepRgaTransformAlive&&) noexcept = default;

    void operator()() const noexcept {}

private:
    std::shared_ptr<RgaTransformLease> lease_;
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
        "RgaModelInputTransformer supports YUYV, NV12 and NV21 source frames");
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

class AlignedRange final {
public:
    AlignedRange(std::uint32_t begin, std::uint32_t end) noexcept
        : begin_(begin), end_(end) {}
    ~AlignedRange() = default;

    AlignedRange(const AlignedRange&) = default;
    AlignedRange& operator=(const AlignedRange&) = default;
    AlignedRange(AlignedRange&&) noexcept = default;
    AlignedRange& operator=(AlignedRange&&) noexcept = default;

    std::uint32_t begin() const noexcept { return begin_; }
    std::uint32_t end() const noexcept { return end_; }

private:
    std::uint32_t begin_ = 0;
    std::uint32_t end_ = 0;
};

AlignedRange expand_aligned_range(
    std::uint32_t begin,
    std::uint32_t end,
    std::uint32_t limit,
    std::uint32_t minimum_size) {
    const std::uint32_t aligned_limit = round_down_even(limit);
    const std::uint32_t aligned_minimum = std::min(
        round_up_even(minimum_size), aligned_limit);
    if (end - begin >= aligned_minimum) {
        return AlignedRange(begin, end);
    }

    const std::uint32_t center = begin + ((end - begin) / 2U);
    std::uint32_t expanded_begin = 0;
    if (center > aligned_minimum / 2U) {
        expanded_begin = center - aligned_minimum / 2U;
    }
    expanded_begin = round_down_even(expanded_begin);
    std::uint32_t expanded_end = expanded_begin + aligned_minimum;
    if (expanded_end > aligned_limit) {
        expanded_end = aligned_limit;
        expanded_begin = expanded_end - aligned_minimum;
    }
    return AlignedRange(expanded_begin, expanded_end);
}

CropRegion rga_crop(
    const FrameView& frame,
    const CropRegion& requested,
    const ModelInputSpec& spec) {
    std::uint32_t left = round_down_even(requested.x());
    std::uint32_t top = round_down_even(requested.y());
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

    constexpr std::uint32_t maximum_upscale_factor = 2U;
    const std::uint32_t minimum_width =
        (spec.width() + maximum_upscale_factor - 1U) /
        maximum_upscale_factor;
    const std::uint32_t minimum_height =
        (spec.height() + maximum_upscale_factor - 1U) /
        maximum_upscale_factor;
    const AlignedRange horizontal = expand_aligned_range(
        left, right, frame.width(), minimum_width);
    const AlignedRange vertical = expand_aligned_range(
        top, bottom, frame.height(), minimum_height);
    left = horizontal.begin();
    right = horizontal.end();
    top = vertical.begin();
    bottom = vertical.end();
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
            "RgaModelInputTransformer produces RGB888 UINT8 NHWC only");
    }
    if (frame.plane_count() != 1) {
        throw std::invalid_argument(
            "RgaModelInputTransformer requires one contiguous source plane");
    }
    if (frame.pixel_format() != VideoPixelFormat::yuyv &&
        frame.pixel_format() != VideoPixelFormat::nv12 &&
        frame.pixel_format() != VideoPixelFormat::nv21) {
        throw std::invalid_argument(
            "RgaModelInputTransformer supports YUYV, NV12 and NV21 source frames");
    }
    const FramePlaneView& plane = frame.plane(0);
    if (frame.memory_type() != FrameMemoryType::dma_buf ||
        plane.dma_buf_fd() < 0) {
        throw std::invalid_argument(
            "RgaModelInputTransformer requires a DMA-BUF source frame");
    }
    if (plane.offset_bytes() != 0) {
        throw std::invalid_argument(
            "RgaModelInputTransformer requires a zero-offset DMA-BUF plane");
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

class RgaModelInputTransformer::Impl final {
public:
    explicit Impl(std::shared_ptr<RgaExecutor> executor)
        : session_(std::move(executor), 2) {}
    ~Impl() { finish(); }

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
                "RGA model-input source generation is still active");
        }
        session_.begin_source(generation.value());
        active_generation_ = generation;
    }

    ModelInputLease prepare(
        const FrameView& frame,
        const CropRegion& crop,
        const ModelInputSpec& spec) {
        validate_request(frame, crop, spec);
        const SourceLayout layout = source_layout(frame);
        const CropRegion effective_crop = rga_crop(frame, crop, spec);
        if (!active_generation_.has_value()) {
            throw std::logic_error(
                "RGA model-input source generation is not active");
        }
        const FramePlaneView& plane = frame.plane(0);
        RgaSourceFrame source(
            plane.dma_buf_fd(),
            plane.size_bytes(),
            static_cast<std::uint64_t>(plane.dma_buf_fd()),
            active_generation_->value(),
            frame.width(),
            frame.height(),
            layout.horizontal_stride(),
            frame.height(),
            plane.stride_bytes(),
            layout.format());
        RgaTransformSpec transform_spec(
            RgaTransformOperation::crop_scale_convert,
            RgaRectangle(
                effective_crop.x(),
                effective_crop.y(),
                effective_crop.width(),
                effective_crop.height()),
            spec.width(),
            spec.height(),
            spec.width(),
            spec.height(),
            spec.width() * 3U,
            spec.minimum_buffer_size_bytes(),
            RgaPixelFormat::rgb888);
        auto transform = std::make_shared<RgaTransformLease>(
            session_.transform(source, transform_spec));
        const std::shared_ptr<RgaBuffer>& output = transform->buffer();

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
            std::move(view), KeepRgaTransformAlive(std::move(transform)));
    }

    void retire_source(VideoStreamGeneration generation) noexcept {
        if (!active_generation_ || *active_generation_ != generation) {
            return;
        }
        session_.retire_source(generation.value());
        active_generation_.reset();
    }

    void finish() noexcept {
        if (active_generation_.has_value()) {
            session_.retire_source(active_generation_->value());
            active_generation_.reset();
        }
        session_.finish();
    }

private:
    RgaTransformSession session_;
    std::optional<VideoStreamGeneration> active_generation_;
};

RgaModelInputTransformer::RgaModelInputTransformer(
    std::shared_ptr<RgaExecutor> executor)
    : impl_(std::make_unique<Impl>(std::move(executor))) {}

RgaModelInputTransformer::~RgaModelInputTransformer() = default;

void RgaModelInputTransformer::begin_source(VideoStreamGeneration generation) {
    impl_->begin_source(generation);
}

ModelInputLease RgaModelInputTransformer::prepare(
    const FrameView& frame,
    const CropRegion& crop,
    const ModelInputSpec& spec) {
    return impl_->prepare(frame, crop, spec);
}

void RgaModelInputTransformer::retire_source(
    VideoStreamGeneration generation) noexcept {
    impl_->retire_source(generation);
}

void RgaModelInputTransformer::finish() noexcept {
    impl_->finish();
}

}  // namespace vtb
