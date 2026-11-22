#include "live2d_tracking_snapshot.hpp"

#include <utility>

namespace vtb {

Live2DTrackingViewState::Live2DTrackingViewState(
    float zoom,
    float offset_x,
    float offset_y)
    : zoom_(zoom), offset_x_(offset_x), offset_y_(offset_y) {}

float Live2DTrackingViewState::zoom() const noexcept {
    return zoom_;
}

float Live2DTrackingViewState::offset_x() const noexcept {
    return offset_x_;
}

float Live2DTrackingViewState::offset_y() const noexcept {
    return offset_y_;
}

Live2DTrackingSnapshot::Live2DTrackingSnapshot(
    std::uint64_t snapshot_generation,
    std::uint64_t source_frame_id,
    Live2DMappingResult mapping,
    Live2DTrackingViewState view)
    : snapshot_generation_(snapshot_generation),
      source_frame_id_(source_frame_id),
      mapping_(std::move(mapping)),
      view_(std::move(view)) {}

std::uint64_t Live2DTrackingSnapshot::snapshot_generation() const noexcept {
    return snapshot_generation_;
}

std::uint64_t Live2DTrackingSnapshot::source_frame_id() const noexcept {
    return source_frame_id_;
}

const Live2DMappingResult& Live2DTrackingSnapshot::mapping() const noexcept {
    return mapping_;
}

const Live2DTrackingViewState& Live2DTrackingSnapshot::view() const noexcept {
    return view_;
}

}  // namespace vtb
