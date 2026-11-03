#pragma once

#include <memory>
#include <optional>

#include "live2d_tracking_snapshot.hpp"
#include "vtb/face/face_pipeline.hpp"
#include "vtb/live2d/live2d_face_mapper.hpp"
#include "vtb/video/video_source.hpp"

namespace vtb {

class Live2DTrackingSession {
public:
    Live2DTrackingSession(
        std::unique_ptr<VideoSource> video_input,
        std::unique_ptr<FacePipeline> face_pipeline,
        Live2DMappingConfig mapping_config);
    ~Live2DTrackingSession();

    Live2DTrackingSession(const Live2DTrackingSession&) = delete;
    Live2DTrackingSession& operator=(const Live2DTrackingSession&) = delete;
    Live2DTrackingSession(Live2DTrackingSession&&) = delete;
    Live2DTrackingSession& operator=(Live2DTrackingSession&&) = delete;

    void start();
    void request_stop() noexcept;
    void stop();
    void rethrow_if_failed() const;
    // Single polling consumer. nullopt means no snapshot update, not tracking loss.
    // A view-only update retains the mapping's original produced timestamp.
    std::optional<Live2DTrackingSnapshot> poll_snapshot_update();
    Live2DTrackingStatus status() const;
    void request_calibration_reset() noexcept;
    bool set_view(const Live2DTrackingViewState& view) noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vtb
