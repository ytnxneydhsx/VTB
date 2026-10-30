#include "vtb/video/latest_frame_slot.hpp"

#include <utility>

namespace vtb {

bool LatestFrameSlot::publish(DecodedFrameLease frame) {
    std::optional<DecodedFrameLease> replaced;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (finished_) {
            return false;
        }
        if (frame_) {
            replaced = std::move(frame_);
            ++replaced_count_;
        }
        frame_ = std::move(frame);
        ++published_count_;
    }
    condition_.notify_one();
    return true;
}

std::optional<DecodedFrameLease> LatestFrameSlot::wait_and_take() {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this]() { return finished_ || frame_.has_value(); });
    if (!frame_) {
        return std::nullopt;
    }
    std::optional<DecodedFrameLease> result = std::move(frame_);
    frame_.reset();
    return result;
}

void LatestFrameSlot::finish() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        finished_ = true;
    }
    condition_.notify_all();
}

void LatestFrameSlot::close() noexcept {
    std::optional<DecodedFrameLease> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        finished_ = true;
        pending = std::move(frame_);
        frame_.reset();
    }
    condition_.notify_all();
}

std::uint64_t LatestFrameSlot::published_count() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return published_count_;
}

std::uint64_t LatestFrameSlot::replaced_count() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return replaced_count_;
}

}  // namespace vtb
