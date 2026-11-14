#include "vtb/messaging/video/decoded_frame_lease.hpp"

#include <utility>

namespace vtb {

DecodedFrameLease::DecodedFrameLease(
    FrameView view,
    std::function<void()> return_buffer)
    : view_(std::move(view)),
      return_buffer_(std::move(return_buffer)),
      valid_(true) {}

DecodedFrameLease::~DecodedFrameLease() {
    reset();
}

DecodedFrameLease::DecodedFrameLease(DecodedFrameLease&& other) noexcept
    : view_(std::move(other.view_)),
      return_buffer_(std::move(other.return_buffer_)),
      valid_(other.valid_) {
    other.valid_ = false;
    other.return_buffer_ = {};
}

DecodedFrameLease& DecodedFrameLease::operator=(
    DecodedFrameLease&& other) noexcept {
    if (this != &other) {
        reset();
        view_ = std::move(other.view_);
        return_buffer_ = std::move(other.return_buffer_);
        valid_ = other.valid_;
        other.valid_ = false;
        other.return_buffer_ = {};
    }
    return *this;
}

const FrameView& DecodedFrameLease::view() const noexcept {
    return view_;
}

DecodedFrameLease::operator bool() const noexcept {
    return valid_;
}

void DecodedFrameLease::reset() noexcept {
    if (valid_ && return_buffer_) {
        try {
            return_buffer_();
        } catch (...) {
        }
    }
    return_buffer_ = {};
    valid_ = false;
}

}  // namespace vtb
