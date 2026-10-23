#include "vtb/frame_ring_buffer.hpp"

#include <cstring>
#include <stdexcept>

namespace vtb {

FrameRingBuffer::FrameRingBuffer(const std::size_t capacity)
    : slots_(capacity) {
    if (capacity == 0) {
        throw std::invalid_argument("frame cache capacity must be greater than zero");
    }
}

std::uint64_t FrameRingBuffer::push(
    FrameMetadata metadata,
    const void* data,
    const std::size_t size) {
    if (size > 0 && data == nullptr) {
        throw std::invalid_argument("frame data is null");
    }

    std::uint64_t sequence = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            return 0;
        }

        Frame& slot = slots_[write_index_];
        metadata.sequence = next_sequence_++;
        slot.metadata = metadata;
        slot.data.resize(size);
        if (size > 0) {
            std::memcpy(slot.data.data(), data, size);
        }

        sequence = metadata.sequence;
        latest_index_ = write_index_;
        write_index_ = (write_index_ + 1) % slots_.size();

        if (stored_count_ < slots_.size()) {
            ++stored_count_;
        } else {
            ++overwrite_count_;
        }
    }
    condition_.notify_all();
    return sequence;
}

std::optional<Frame> FrameRingBuffer::latest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stored_count_ == 0) {
        return std::nullopt;
    }
    return slots_[latest_index_];
}

std::optional<Frame> FrameRingBuffer::waitForLatest(
    const std::uint64_t after_sequence,
    const std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait_for(lock, timeout, [this, after_sequence] {
        return closed_ ||
               (stored_count_ > 0 &&
                slots_[latest_index_].metadata.sequence > after_sequence);
    });

    if (stored_count_ == 0 ||
        slots_[latest_index_].metadata.sequence <= after_sequence) {
        return std::nullopt;
    }
    return slots_[latest_index_];
}

void FrameRingBuffer::close() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }
    condition_.notify_all();
}

bool FrameRingBuffer::closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
}

std::size_t FrameRingBuffer::capacity() const noexcept {
    return slots_.size();
}

std::size_t FrameRingBuffer::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stored_count_;
}

std::uint64_t FrameRingBuffer::overwriteCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return overwrite_count_;
}

}  // namespace vtb
