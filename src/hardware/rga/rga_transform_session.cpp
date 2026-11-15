#include "vtb/hardware/rga/rga_transform_session.hpp"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

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
    if (spec.operation() == RgaTransformOperation::rgba_to_nv12 &&
        (source.format() != RgaPixelFormat::rgba8888 ||
         spec.destination_format() != RgaPixelFormat::nv12)) {
        throw std::invalid_argument(
            "RGBA-to-NV12 operation requires RGBA8888 and NV12 formats");
    }
}

class DestinationSpec final {
public:
    explicit DestinationSpec(const RgaTransformSpec& spec)
        : width_(spec.destination_width()),
          height_(spec.destination_height()),
          horizontal_stride_(spec.destination_horizontal_stride()),
          vertical_stride_(spec.destination_vertical_stride()),
          stride_bytes_(spec.destination_stride_bytes()),
          minimum_size_bytes_(spec.destination_minimum_size_bytes()),
          format_(spec.destination_format()) {}
    ~DestinationSpec() = default;

    DestinationSpec(const DestinationSpec&) = default;
    DestinationSpec& operator=(const DestinationSpec&) = default;
    DestinationSpec(DestinationSpec&&) noexcept = default;
    DestinationSpec& operator=(DestinationSpec&&) noexcept = default;

    bool matches(const DestinationSpec& other) const noexcept {
        return width_ == other.width_ && height_ == other.height_ &&
            horizontal_stride_ == other.horizontal_stride_ &&
            vertical_stride_ == other.vertical_stride_ &&
            stride_bytes_ == other.stride_bytes_ &&
            minimum_size_bytes_ == other.minimum_size_bytes_ &&
            format_ == other.format_;
    }

    std::uint32_t width() const noexcept { return width_; }
    std::uint32_t height() const noexcept { return height_; }
    std::uint32_t horizontal_stride() const noexcept {
        return horizontal_stride_;
    }
    std::uint32_t vertical_stride() const noexcept {
        return vertical_stride_;
    }
    std::uint32_t stride_bytes() const noexcept { return stride_bytes_; }
    std::size_t minimum_size_bytes() const noexcept {
        return minimum_size_bytes_;
    }
    RgaPixelFormat format() const noexcept { return format_; }

private:
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t horizontal_stride_ = 0;
    std::uint32_t vertical_stride_ = 0;
    std::uint32_t stride_bytes_ = 0;
    std::size_t minimum_size_bytes_ = 0;
    RgaPixelFormat format_ = RgaPixelFormat::rgb888;
};

class DestinationSlot final {
public:
    explicit DestinationSlot(std::shared_ptr<RgaBuffer> buffer)
        : buffer_(std::move(buffer)) {}
    ~DestinationSlot() = default;

    DestinationSlot(const DestinationSlot&) = delete;
    DestinationSlot& operator=(const DestinationSlot&) = delete;
    DestinationSlot(DestinationSlot&&) noexcept = default;
    DestinationSlot& operator=(DestinationSlot&&) noexcept = default;

    const std::shared_ptr<RgaBuffer>& buffer() const noexcept {
        return buffer_;
    }
    bool in_use() const noexcept { return in_use_; }
    void set_in_use(bool in_use) noexcept { in_use_ = in_use; }

private:
    std::shared_ptr<RgaBuffer> buffer_;
    bool in_use_ = false;
};

class DestinationPool;

class DestinationSlotToken final {
public:
    DestinationSlotToken(
        std::shared_ptr<DestinationPool> pool,
        std::size_t index)
        : pool_(std::move(pool)), index_(index) {}
    ~DestinationSlotToken();

    DestinationSlotToken(const DestinationSlotToken&) = delete;
    DestinationSlotToken& operator=(const DestinationSlotToken&) = delete;
    DestinationSlotToken(DestinationSlotToken&&) = delete;
    DestinationSlotToken& operator=(DestinationSlotToken&&) = delete;

private:
    std::shared_ptr<DestinationPool> pool_;
    std::size_t index_ = 0;
};

class SlotReservation final {
public:
    SlotReservation(
        std::shared_ptr<RgaBuffer> buffer,
        std::size_t index,
        std::shared_ptr<void> token)
        : buffer_(std::move(buffer)),
          index_(index),
          token_(std::move(token)) {}
    ~SlotReservation() = default;

    SlotReservation(const SlotReservation&) = delete;
    SlotReservation& operator=(const SlotReservation&) = delete;
    SlotReservation(SlotReservation&&) noexcept = default;
    SlotReservation& operator=(SlotReservation&&) noexcept = default;

    const std::shared_ptr<RgaBuffer>& buffer() const noexcept {
        return buffer_;
    }
    std::uint64_t buffer_id() const noexcept {
        return static_cast<std::uint64_t>(index_ + 1U);
    }
    std::shared_ptr<void> take_token() noexcept {
        return std::move(token_);
    }

private:
    std::shared_ptr<RgaBuffer> buffer_;
    std::size_t index_ = 0;
    std::shared_ptr<void> token_;
};

class DestinationPool final :
    public std::enable_shared_from_this<DestinationPool> {
public:
    DestinationPool(
        std::string dma_heap_path,
        std::size_t capacity)
        : dma_heap_path_(std::move(dma_heap_path)), capacity_(capacity) {
        if (dma_heap_path_.empty() || capacity_ == 0) {
            throw std::invalid_argument(
                "RGA destination pool configuration is invalid");
        }
    }
    ~DestinationPool() = default;

    DestinationPool(const DestinationPool&) = delete;
    DestinationPool& operator=(const DestinationPool&) = delete;
    DestinationPool(DestinationPool&&) = delete;
    DestinationPool& operator=(DestinationPool&&) = delete;

    SlotReservation acquire(const RgaTransformSpec& transform_spec) {
        const DestinationSpec requested(transform_spec);
        std::unique_lock<std::mutex> lock(mutex_);
        if (!spec_.has_value() || !spec_->matches(requested)) {
            available_.wait(lock, [this]() { return all_slots_free(); });
            build_slots(requested);
        }
        available_.wait(lock, [this]() { return has_free_slot(); });
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            if (!slots_[index].in_use()) {
                slots_[index].set_in_use(true);
                std::shared_ptr<void> token =
                    std::make_shared<DestinationSlotToken>(
                        shared_from_this(), index);
                return SlotReservation(
                    slots_[index].buffer(), index, std::move(token));
            }
        }
        throw std::logic_error("RGA destination pool lost a free slot");
    }

    void release(std::size_t index) noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (index >= slots_.size()) {
                return;
            }
            slots_[index].set_in_use(false);
        }
        available_.notify_all();
    }

private:
    bool has_free_slot() const noexcept {
        for (const DestinationSlot& slot : slots_) {
            if (!slot.in_use()) {
                return true;
            }
        }
        return false;
    }

    bool all_slots_free() const noexcept {
        for (const DestinationSlot& slot : slots_) {
            if (slot.in_use()) {
                return false;
            }
        }
        return true;
    }

    void build_slots(const DestinationSpec& spec) {
        std::vector<DestinationSlot> replacement;
        replacement.reserve(capacity_);
        for (std::size_t index = 0; index < capacity_; ++index) {
            replacement.emplace_back(RgaDevice::allocate_buffer(
                dma_heap_path_,
                spec.width(),
                spec.height(),
                spec.horizontal_stride(),
                spec.vertical_stride(),
                spec.stride_bytes(),
                spec.minimum_size_bytes(),
                spec.format()));
        }
        slots_ = std::move(replacement);
        spec_ = spec;
    }

    std::string dma_heap_path_;
    std::size_t capacity_ = 0;
    mutable std::mutex mutex_;
    std::condition_variable available_;
    std::optional<DestinationSpec> spec_;
    std::vector<DestinationSlot> slots_;
};

DestinationSlotToken::~DestinationSlotToken() {
    pool_->release(index_);
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

RgaTransformLease::RgaTransformLease(
    std::shared_ptr<RgaBuffer> buffer,
    std::uint64_t buffer_id,
    std::shared_ptr<void> slot_token)
    : buffer_(std::move(buffer)),
      buffer_id_(buffer_id),
      slot_token_(std::move(slot_token)) {}

const std::shared_ptr<RgaBuffer>&
RgaTransformLease::buffer() const noexcept {
    return buffer_;
}

std::uint64_t RgaTransformLease::buffer_id() const noexcept {
    return buffer_id_;
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
        destination_pool_ = std::make_shared<DestinationPool>(
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
        SlotReservation destination = destination_pool_->acquire(spec);
        RgaTransformRequest request(
            spec.operation(),
            source_buffer,
            destination.buffer(),
            spec.source_rectangle());
        transformer_.transform(request);
        const std::uint64_t buffer_id = destination.buffer_id();
        std::shared_ptr<RgaBuffer> buffer = destination.buffer();
        std::shared_ptr<void> token = destination.take_token();
        return RgaTransformLease(
            std::move(buffer), buffer_id, std::move(token));
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
    std::shared_ptr<DestinationPool> destination_pool_;
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
