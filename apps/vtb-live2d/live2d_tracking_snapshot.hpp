#pragma once

#include <cstdint>

#include "vtb/live2d/live2d_mapping_result.hpp"

namespace vtb {

/** @brief Live2D 取景缩放和偏移数据。 */
class Live2DTrackingViewState {
public:
    Live2DTrackingViewState() = default;
    Live2DTrackingViewState(float zoom, float offset_x, float offset_y);
    ~Live2DTrackingViewState() = default;

    Live2DTrackingViewState(const Live2DTrackingViewState&) = default;
    Live2DTrackingViewState& operator=(const Live2DTrackingViewState&) = default;
    Live2DTrackingViewState(Live2DTrackingViewState&&) noexcept = default;
    Live2DTrackingViewState& operator=(Live2DTrackingViewState&&) noexcept = default;

    float zoom() const noexcept;
    float offset_x() const noexcept;
    float offset_y() const noexcept;

private:
    float zoom_ = 1.0F;
    float offset_x_ = 0.0F;
    float offset_y_ = 0.0F;
};

/** @brief Godot 所有者线程取得的参数和取景快照；版本随任一状态变化递增。 */
class Live2DTrackingSnapshot {
public:
    Live2DTrackingSnapshot(
        std::uint64_t snapshot_generation,
        Live2DMappingResult mapping,
        Live2DTrackingViewState view);
    ~Live2DTrackingSnapshot() = default;

    Live2DTrackingSnapshot(const Live2DTrackingSnapshot&) = default;
    Live2DTrackingSnapshot& operator=(const Live2DTrackingSnapshot&) = default;
    Live2DTrackingSnapshot(Live2DTrackingSnapshot&&) noexcept = default;
    Live2DTrackingSnapshot& operator=(Live2DTrackingSnapshot&&) noexcept = default;

    std::uint64_t snapshot_generation() const noexcept;
    const Live2DMappingResult& mapping() const noexcept;
    const Live2DTrackingViewState& view() const noexcept;

private:
    std::uint64_t snapshot_generation_ = 0;
    Live2DMappingResult mapping_;
    Live2DTrackingViewState view_;
};

/** @brief An atomic status read for the application's control endpoint. */
class Live2DTrackingStatus {
public:
    bool calibrated = false;
    bool tracking_valid = false;
    std::size_t calibration_progress = 0;
    std::size_t calibration_required = 0;
    float confidence = 0.0F;
    Live2DTrackingViewState view;
};

}  // namespace vtb
