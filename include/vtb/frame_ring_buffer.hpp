#pragma once

#include "vtb/frame.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace vtb {

// Thread-safe, fixed-slot frame cache. A producer overwrites the oldest slot
// after the cache becomes full. Readers receive an owned snapshot so a slot can
// be safely reused while downstream processing continues.
class FrameRingBuffer {
public:
    explicit FrameRingBuffer(std::size_t capacity);

    FrameRingBuffer(const FrameRingBuffer&) = delete;
    FrameRingBuffer& operator=(const FrameRingBuffer&) = delete;

    // Copies one driver-owned frame into the next cache slot and assigns a
    // monotonically increasing application sequence number. Returns zero when
    // the cache has already been closed.
    std::uint64_t push(
        FrameMetadata metadata,
        const void* data,
        std::size_t size);

    // Returns a copy of the newest frame currently in the cache.
    std::optional<Frame> latest() const;

    // Waits until a frame newer than after_sequence exists, the timeout
    // expires, or close() is called. A returned frame is an owned copy.
    std::optional<Frame> waitForLatest(
        std::uint64_t after_sequence,
        std::chrono::milliseconds timeout) const;

    void close();
    bool closed() const;

    std::size_t capacity() const noexcept;
    std::size_t size() const;
    std::uint64_t overwriteCount() const;

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;
    std::vector<Frame> slots_;
    std::size_t write_index_ = 0;
    std::size_t stored_count_ = 0;
    std::size_t latest_index_ = 0;
    std::uint64_t next_sequence_ = 1;
    std::uint64_t overwrite_count_ = 0;
    bool closed_ = false;
};

}  // namespace vtb
