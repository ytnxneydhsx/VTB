#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>

#include "vtb/video/frame.hpp"

namespace vtb {

class LatestFrameSlot {
public:
    LatestFrameSlot() = default;
    ~LatestFrameSlot() = default;

    LatestFrameSlot(const LatestFrameSlot&) = delete;
    LatestFrameSlot& operator=(const LatestFrameSlot&) = delete;
    LatestFrameSlot(LatestFrameSlot&&) = delete;
    LatestFrameSlot& operator=(LatestFrameSlot&&) = delete;

    bool publish(DecodedFrameLease frame);
    std::optional<DecodedFrameLease> wait_and_take();
    void finish() noexcept;
    void close() noexcept;

    std::uint64_t published_count() const noexcept;
    std::uint64_t replaced_count() const noexcept;

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<DecodedFrameLease> frame_;
    bool finished_ = false;
    std::uint64_t published_count_ = 0;
    std::uint64_t replaced_count_ = 0;
};

}  // namespace vtb
