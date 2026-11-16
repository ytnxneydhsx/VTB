#include "vtb/hardware/rga/rga_buffer_pool.hpp"

#include <condition_variable>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "vtb/hardware/rga/rga_device.hpp"
#include "vtb/hardware/rga/rga_transform_session.hpp"

namespace vtb {
namespace {

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

class SlotReservation final {
public:
    SlotReservation(
        std::shared_ptr<RgaBuffer> buffer,
        std::size_t index,
        std::shared_ptr<void> return_token) noexcept
        : buffer_(std::move(buffer)),
          index_(index),
          return_token_(std::move(return_token)) {}
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
    std::shared_ptr<void> take_return_token() noexcept {
        return std::move(return_token_);
    }

private:
    std::shared_ptr<RgaBuffer> buffer_;
    std::size_t index_ = 0;
    std::shared_ptr<void> return_token_;
};

}  // namespace

RgaTransformLease::RgaTransformLease(
    std::shared_ptr<RgaBuffer> buffer,
    std::uint64_t buffer_id,
    std::shared_ptr<void> return_token)
    : buffer_(std::move(buffer)),
      buffer_id_(buffer_id),
      return_token_(std::move(return_token)) {}

const std::shared_ptr<RgaBuffer>&
RgaTransformLease::buffer() const noexcept {
    return buffer_;
}

std::uint64_t RgaTransformLease::buffer_id() const noexcept {
    return buffer_id_;
}

class RgaBufferPool::Impl final {
public:
    Impl(std::string dma_heap_path, std::size_t capacity)
        : dma_heap_path_(std::move(dma_heap_path)), capacity_(capacity) {
        if (dma_heap_path_.empty() || capacity_ == 0) {
            throw std::invalid_argument(
                "RGA buffer pool configuration is invalid");
        }
    }
    ~Impl() = default;

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    SlotReservation acquire(
        const RgaTransformSpec& transform_spec,
        const std::shared_ptr<RgaBufferPool>& owner) {
        const DestinationSpec requested(transform_spec);
        std::unique_lock<std::mutex> lock(mutex_);
        if (!spec_.has_value() || !spec_->matches(requested)) {
            available_.wait(lock, [this]() { return all_slots_free(); });
            build_slots(requested);
        }
        available_.wait(lock, [this]() { return has_free_slot(); });
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            if (!slots_[index].in_use()) {
                std::shared_ptr<void> return_token =
                    std::make_shared<ReleaseOnLeaseDestroy>(owner, index);
                slots_[index].set_in_use(true);
                SlotReservation reservation(
                    slots_[index].buffer(),
                    index,
                    std::move(return_token));
                lock.unlock();
                return reservation;
            }
        }
        throw std::logic_error("RGA buffer pool lost a free slot");
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

RgaBufferPool::ReleaseOnLeaseDestroy::ReleaseOnLeaseDestroy(
    std::shared_ptr<RgaBufferPool> owner,
    std::size_t index) noexcept
    : owner_(std::move(owner)), index_(index) {}

RgaBufferPool::ReleaseOnLeaseDestroy::~ReleaseOnLeaseDestroy() {
    owner_->release(index_);
}

RgaBufferPool::RgaBufferPool(
    std::string dma_heap_path,
    std::size_t capacity)
    : impl_(std::make_unique<Impl>(std::move(dma_heap_path), capacity)) {}

RgaBufferPool::~RgaBufferPool() = default;

RgaTransformLease RgaBufferPool::acquire(
    const RgaTransformSpec& transform_spec) {
    SlotReservation reservation =
        impl_->acquire(transform_spec, shared_from_this());
    return RgaTransformLease(
        reservation.buffer(),
        reservation.buffer_id(),
        reservation.take_return_token());
}

void RgaBufferPool::release(std::size_t index) noexcept {
    impl_->release(index);
}

}  // namespace vtb
