#include "vtb/hardware/rga/rga_transform_session.hpp"

#include <algorithm>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>
#include "vtb/trace/frame_trace.hpp"

namespace vtb {
namespace {

void validate_source_frame(const RgaSourceFrame& source) {
    if (source.dma_buf_fd() < 0 || source.size_bytes() == 0 ||
        source.stream_generation() == 0 || source.width() == 0 ||
        source.height() == 0 ||
        source.horizontal_stride() < source.width() ||
        source.vertical_stride() < source.height() ||
        source.stride_bytes() == 0) {
        throw std::invalid_argument("RGA source frame is invalid");
    }
    std::uint32_t minimum_stride = source.horizontal_stride();
    if (source.format() == RgaPixelFormat::yuyv422) {
        minimum_stride *= 2U;
    } else if (source.format() == RgaPixelFormat::rgb888) {
        minimum_stride *= 3U;
    } else if (source.format() == RgaPixelFormat::rgba8888) {
        minimum_stride *= 4U;
    }
    if (source.stride_bytes() < minimum_stride) {
        throw std::invalid_argument(
            "RGA source byte stride is smaller than its pixel stride");
    }
}

void validate_transform_spec(
    const RgaSourceFrame& source,
    const RgaTransformSpec& spec) {
    const RgaRectangle& rectangle = spec.source_rectangle();
    if (rectangle.width() == 0 || rectangle.height() == 0 ||
        rectangle.x() + rectangle.width() > source.width() ||
        rectangle.y() + rectangle.height() > source.height() ||
        spec.destination_width() == 0 ||
        spec.destination_height() == 0 ||
        spec.destination_horizontal_stride() < spec.destination_width() ||
        spec.destination_vertical_stride() < spec.destination_height() ||
        spec.destination_stride_bytes() == 0) {
        throw std::invalid_argument("RGA transform specification is invalid");
    }
    const bool converts_rgba_to_nv12 =
        spec.operation() == RgaTransformOperation::rgba_to_nv12 ||
        spec.operation() ==
            RgaTransformOperation::rgba_to_nv12_flip_vertical;
    if (converts_rgba_to_nv12 &&
        (source.format() != RgaPixelFormat::rgba8888 ||
         spec.destination_format() != RgaPixelFormat::nv12)) {
        throw std::invalid_argument(
            "RGBA-to-NV12 operation requires RGBA8888 and NV12 formats");
    }
}

class SourceRegistration final {
public:
    SourceRegistration(
        RgaSourceFrame source,
        std::shared_ptr<RgaBuffer> buffer)
        : source_(std::move(source)), buffer_(std::move(buffer)) {}
    ~SourceRegistration() = default;

    SourceRegistration(const SourceRegistration&) = default;
    SourceRegistration& operator=(const SourceRegistration&) = default;
    SourceRegistration(SourceRegistration&&) noexcept = default;
    SourceRegistration& operator=(SourceRegistration&&) noexcept = default;

    bool matches(const RgaSourceFrame& source) const noexcept {
        return same_identity(source) &&
            source_.size_bytes() == source.size_bytes() &&
            source_.width() == source.width() &&
            source_.height() == source.height() &&
            source_.horizontal_stride() == source.horizontal_stride() &&
            source_.vertical_stride() == source.vertical_stride() &&
            source_.stride_bytes() == source.stride_bytes() &&
            source_.format() == source.format();
    }

    bool same_identity(const RgaSourceFrame& source) const noexcept {
        return source_.dma_buf_fd() == source.dma_buf_fd() &&
            source_.buffer_id() == source.buffer_id() &&
            source_.stream_generation() == source.stream_generation();
    }

    const std::shared_ptr<RgaBuffer>& buffer() const noexcept {
        return buffer_;
    }

private:
    RgaSourceFrame source_;
    std::shared_ptr<RgaBuffer> buffer_;
};

}  // namespace

RgaSourceFrame::RgaSourceFrame(
    int dma_buf_fd,
    std::size_t size_bytes,
    std::uint64_t buffer_id,
    std::uint64_t stream_generation,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t horizontal_stride,
    std::uint32_t vertical_stride,
    std::uint32_t stride_bytes,
    RgaPixelFormat format)
    : dma_buf_fd_(dma_buf_fd),
      size_bytes_(size_bytes),
      buffer_id_(buffer_id),
      stream_generation_(stream_generation),
      width_(width),
      height_(height),
      horizontal_stride_(horizontal_stride),
      vertical_stride_(vertical_stride),
      stride_bytes_(stride_bytes),
      format_(format) {}

int RgaSourceFrame::dma_buf_fd() const noexcept { return dma_buf_fd_; }
std::size_t RgaSourceFrame::size_bytes() const noexcept { return size_bytes_; }
std::uint64_t RgaSourceFrame::buffer_id() const noexcept { return buffer_id_; }
std::uint64_t RgaSourceFrame::stream_generation() const noexcept {
    return stream_generation_;
}
std::uint32_t RgaSourceFrame::width() const noexcept { return width_; }
std::uint32_t RgaSourceFrame::height() const noexcept { return height_; }
std::uint32_t RgaSourceFrame::horizontal_stride() const noexcept {
    return horizontal_stride_;
}
std::uint32_t RgaSourceFrame::vertical_stride() const noexcept {
    return vertical_stride_;
}
std::uint32_t RgaSourceFrame::stride_bytes() const noexcept {
    return stride_bytes_;
}
RgaPixelFormat RgaSourceFrame::format() const noexcept { return format_; }

RgaTransformSpec::RgaTransformSpec(
    RgaTransformOperation operation,
    RgaRectangle source_rectangle,
    std::uint32_t destination_width,
    std::uint32_t destination_height,
    std::uint32_t destination_horizontal_stride,
    std::uint32_t destination_vertical_stride,
    std::uint32_t destination_stride_bytes,
    std::size_t destination_minimum_size_bytes,
    RgaPixelFormat destination_format)
    : operation_(operation),
      source_rectangle_(source_rectangle),
      destination_width_(destination_width),
      destination_height_(destination_height),
      destination_horizontal_stride_(destination_horizontal_stride),
      destination_vertical_stride_(destination_vertical_stride),
      destination_stride_bytes_(destination_stride_bytes),
      destination_minimum_size_bytes_(destination_minimum_size_bytes),
      destination_format_(destination_format) {}

RgaTransformOperation RgaTransformSpec::operation() const noexcept {
    return operation_;
}
const RgaRectangle& RgaTransformSpec::source_rectangle() const noexcept {
    return source_rectangle_;
}
std::uint32_t RgaTransformSpec::destination_width() const noexcept {
    return destination_width_;
}
std::uint32_t RgaTransformSpec::destination_height() const noexcept {
    return destination_height_;
}
std::uint32_t RgaTransformSpec::destination_horizontal_stride() const noexcept {
    return destination_horizontal_stride_;
}
std::uint32_t RgaTransformSpec::destination_vertical_stride() const noexcept {
    return destination_vertical_stride_;
}
std::uint32_t RgaTransformSpec::destination_stride_bytes() const noexcept {
    return destination_stride_bytes_;
}
std::size_t RgaTransformSpec::destination_minimum_size_bytes() const noexcept {
    return destination_minimum_size_bytes_;
}
RgaPixelFormat RgaTransformSpec::destination_format() const noexcept {
    return destination_format_;
}

class RgaTransformSession::Impl final {
public:
    Impl(
        std::string dma_heap_path,
        std::size_t destination_pool_capacity)
        : destination_pool_capacity_(destination_pool_capacity) {
        if (dma_heap_path.empty() || destination_pool_capacity_ < 2) {
            throw std::invalid_argument(
                "RGA transform session requires a DMA heap and at least two output slots");
        }
        destination_pool_ = std::make_shared<RgaBufferPool>(
            std::move(dma_heap_path), destination_pool_capacity_);
    }

    ~Impl() {
        finish();
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void begin_source(std::uint64_t stream_generation) {
        if (stream_generation == 0) {
            throw std::invalid_argument(
                "RGA source generation must be non-zero");
        }
        std::lock_guard<std::mutex> lock(operation_mutex_);
        if (finished_) {
            throw std::logic_error("RGA transform session is finished");
        }
        if (active_generation_.has_value()) {
            if (*active_generation_ == stream_generation) {
                return;
            }
            throw std::logic_error(
                "Previous RGA source generation is still active");
        }
        active_generation_ = stream_generation;
    }

    RgaTransformLease transform(
        const RgaSourceFrame& source,
        const RgaTransformSpec& spec) {
        std::lock_guard<std::mutex> lock(operation_mutex_);
        if (finished_) {
            throw std::logic_error("RGA transform session is finished");
        }
        validate_source_frame(source);
        validate_transform_spec(source, spec);
        if (!active_generation_.has_value() ||
            *active_generation_ != source.stream_generation()) {
            throw std::logic_error(
                "RGA source generation is not active in this session");
        }

        const std::shared_ptr<RgaBuffer> source_buffer =
            source_buffer_for(source);
        RgaTransformLease destination = [&] {
            VTB_FRAME_SLICE("rga.acquire_destination", frame_trace_track());
            return destination_pool_->acquire(spec);
        }();
        RgaTransformRequest request(
            spec.operation(),
            source_buffer,
            destination.buffer(),
            spec.source_rectangle());
        transformer_.transform(request);
        return destination;
    }

    void retire_source(std::uint64_t stream_generation) noexcept {
        try {
            std::lock_guard<std::mutex> lock(operation_mutex_);
            if (!active_generation_.has_value() ||
                *active_generation_ != stream_generation) {
                return;
            }
            source_buffers_.clear();
            active_generation_.reset();
        } catch (...) {
        }
    }

    void finish() noexcept {
        try {
            std::lock_guard<std::mutex> lock(operation_mutex_);
            if (finished_) {
                return;
            }
            source_buffers_.clear();
            active_generation_.reset();
            destination_pool_.reset();
            finished_ = true;
        } catch (...) {
        }
    }

private:
    std::shared_ptr<RgaBuffer> source_buffer_for(
        const RgaSourceFrame& source) {
        for (const SourceRegistration& registration : source_buffers_) {
            if (registration.matches(source)) {
                return registration.buffer();
            }
        }
        source_buffers_.erase(
            std::remove_if(
                source_buffers_.begin(),
                source_buffers_.end(),
                [&source](const SourceRegistration& registration) {
                    return registration.same_identity(source);
                }),
            source_buffers_.end());
        std::shared_ptr<RgaBuffer> buffer = RgaDevice::register_dma_buf(
            RgaBufferDescription(
                source.dma_buf_fd(),
                source.size_bytes(),
                source.width(),
                source.height(),
                source.horizontal_stride(),
                source.vertical_stride(),
                source.stride_bytes(),
                source.format()));
        source_buffers_.emplace_back(source, buffer);
        return buffer;
    }

    RgaTransformer transformer_;
    std::size_t destination_pool_capacity_ = 0;
    std::shared_ptr<RgaBufferPool> destination_pool_;
    std::mutex operation_mutex_;
    std::optional<std::uint64_t> active_generation_;
    std::vector<SourceRegistration> source_buffers_;
    bool finished_ = false;
};

RgaTransformSession::RgaTransformSession(
    std::string dma_heap_path,
    std::size_t destination_pool_capacity)
    : impl_(std::make_unique<Impl>(
          std::move(dma_heap_path), destination_pool_capacity)) {}

RgaTransformSession::~RgaTransformSession() = default;

void RgaTransformSession::begin_source(std::uint64_t stream_generation) {
    impl_->begin_source(stream_generation);
}

RgaTransformLease RgaTransformSession::transform(
    const RgaSourceFrame& source,
    const RgaTransformSpec& spec) {
    return impl_->transform(source, spec);
}

void RgaTransformSession::retire_source(
    std::uint64_t stream_generation) noexcept {
    impl_->retire_source(stream_generation);
}

void RgaTransformSession::finish() noexcept {
    impl_->finish();
}

}  // namespace vtb
