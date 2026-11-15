#include "vtb/hardware/rga/rga_nv12_frame_transformer.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace vtb {
namespace {

constexpr std::size_t kNv12PoolCapacity = 4;

std::size_t checked_size(std::uint64_t size) {
    if (size == 0 ||
        size > static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max())) {
        throw std::invalid_argument("RGBA DMA-BUF size is invalid");
    }
    return static_cast<std::size_t>(size);
}

void validate_frame(const RgbaFrameView& frame) {
    if (frame.dma_buf_fd < 0 || frame.dma_buf_size == 0 ||
        frame.stream_generation == 0 || frame.width == 0 ||
        frame.height == 0 || frame.horizontal_stride < frame.width ||
        frame.vertical_stride < frame.height ||
        frame.stride_bytes < frame.horizontal_stride * 4U ||
        frame.frames_per_second == 0 || frame.timestamp_ns == 0 ||
        frame.sequence == 0 ||
        frame.format != RgbaFrameFormat::rgba8888) {
        throw std::invalid_argument("RGBA frame view is invalid");
    }
    if ((frame.width & 1U) != 0U || (frame.height & 1U) != 0U) {
        throw std::invalid_argument(
            "RGBA dimensions must be even for NV12 conversion");
    }
    if (frame.end_of_stream != 0 && frame.end_of_stream != 1) {
        throw std::invalid_argument(
            "RGBA end-of-stream flag must be zero or one");
    }
}

}  // namespace

class RgaNv12FrameTransformer::Impl final {
public:
    explicit Impl(std::string dma_heap_path)
        : session_(std::move(dma_heap_path), kNv12PoolCapacity) {}
    ~Impl() { finish(); }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    Nv12FrameLease transform(const RgbaFrameView& frame) {
        validate_frame(frame);
        if (finished_) {
            throw std::logic_error("RGA NV12 transformer is finished");
        }
        if (end_of_stream_received_) {
            throw std::logic_error(
                "RGA NV12 transformer received a frame after end-of-stream");
        }
        if (active_generation_ == 0) {
            session_.begin_source(frame.stream_generation);
            active_generation_ = frame.stream_generation;
        }
        if (active_generation_ != frame.stream_generation) {
            throw std::invalid_argument(
                "RGBA stream generation changed without retirement");
        }
        if (frame.sequence != transformed_frames_ + 1) {
            throw std::invalid_argument(
                "RGBA frame sequence is not contiguous");
        }

        RgaSourceFrame source(
            frame.dma_buf_fd,
            checked_size(frame.dma_buf_size),
            frame.buffer_id,
            frame.stream_generation,
            frame.width,
            frame.height,
            frame.horizontal_stride,
            frame.vertical_stride,
            frame.stride_bytes,
            RgaPixelFormat::rgba8888);
        const std::uint32_t nv12_stride = frame.width;
        const std::uint32_t nv12_vertical_stride =
            (frame.height + 15U) & ~15U;
        const std::size_t nv12_size =
            static_cast<std::size_t>(nv12_stride) *
            static_cast<std::size_t>(nv12_vertical_stride) * 3U / 2U;
        RgaTransformSpec spec(
            RgaTransformOperation::rgba_to_nv12,
            RgaRectangle(0, 0, frame.width, frame.height),
            frame.width,
            frame.height,
            nv12_stride,
            nv12_vertical_stride,
            nv12_stride,
            nv12_size,
            RgaPixelFormat::nv12);
        auto transform = std::make_shared<RgaTransformLease>(
            session_.transform(source, spec));
        const std::shared_ptr<RgaBuffer>& output = transform->buffer();

        Nv12FrameView view;
        view.dma_buf_fd = output->dma_buf_fd();
        view.dma_buf_size = output->size_bytes();
        view.buffer_id = transform->buffer_id();
        view.stream_generation = frame.stream_generation;
        view.width = frame.width;
        view.height = frame.height;
        view.horizontal_stride = nv12_stride;
        view.vertical_stride = nv12_vertical_stride;
        view.frames_per_second = frame.frames_per_second;
        view.timestamp_ns = frame.timestamp_ns;
        view.sequence = frame.sequence;
        view.end_of_stream = frame.end_of_stream;
        ++transformed_frames_;
        if (frame.end_of_stream != 0) {
            end_of_stream_received_ = true;
        }
        return Nv12FrameLease(view, std::move(transform));
    }

    void finish() noexcept {
        if (finished_) {
            return;
        }
        if (active_generation_ != 0) {
            session_.retire_source(active_generation_);
            active_generation_ = 0;
        }
        session_.finish();
        finished_ = true;
    }

private:
    RgaTransformSession session_;
    std::uint64_t active_generation_ = 0;
    std::uint64_t transformed_frames_ = 0;
    bool end_of_stream_received_ = false;
    bool finished_ = false;
};

RgaNv12FrameTransformer::RgaNv12FrameTransformer(
    std::string dma_heap_path)
    : impl_(std::make_unique<Impl>(std::move(dma_heap_path))) {}

RgaNv12FrameTransformer::~RgaNv12FrameTransformer() = default;

Nv12FrameLease RgaNv12FrameTransformer::transform(
    const RgbaFrameView& frame) {
    return impl_->transform(frame);
}

void RgaNv12FrameTransformer::finish() noexcept {
    impl_->finish();
}

}  // namespace vtb
