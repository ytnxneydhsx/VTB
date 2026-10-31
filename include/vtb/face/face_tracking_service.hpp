#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "vtb/face/face_pipeline.hpp"
#include "vtb/live2d/live2d_face_mapper.hpp"
#include "vtb/video/video_source.hpp"

namespace vtb {

class FaceTrackingViewState {
public:
    FaceTrackingViewState() = default;
    FaceTrackingViewState(float zoom, float offset_x, float offset_y);
    ~FaceTrackingViewState() = default;

    FaceTrackingViewState(const FaceTrackingViewState&) = default;
    FaceTrackingViewState& operator=(const FaceTrackingViewState&) = default;
    FaceTrackingViewState(FaceTrackingViewState&&) noexcept = default;
    FaceTrackingViewState& operator=(FaceTrackingViewState&&) noexcept = default;

    float zoom() const noexcept;
    float offset_x() const noexcept;
    float offset_y() const noexcept;

private:
    float zoom_ = 1.0F;
    float offset_x_ = 0.0F;
    float offset_y_ = 0.0F;
};

class FaceTrackingSnapshot {
public:
    FaceTrackingSnapshot(
        std::uint64_t generation,
        Live2DMappingResult mapping,
        FaceTrackingViewState view);
    ~FaceTrackingSnapshot() = default;

    FaceTrackingSnapshot(const FaceTrackingSnapshot&) = default;
    FaceTrackingSnapshot& operator=(const FaceTrackingSnapshot&) = default;
    FaceTrackingSnapshot(FaceTrackingSnapshot&&) noexcept = default;
    FaceTrackingSnapshot& operator=(FaceTrackingSnapshot&&) noexcept = default;

    std::uint64_t generation() const noexcept;
    const Live2DMappingResult& mapping() const noexcept;
    const FaceTrackingViewState& view() const noexcept;

private:
    std::uint64_t generation_ = 0;
    Live2DMappingResult mapping_;
    FaceTrackingViewState view_;
};

class FaceTrackingControlConfig {
public:
    std::string bind_address = "192.168.77.2";
    int port = 11575;
    std::string allowed_origin = "http://192.168.77.1:8900";
};

class FaceTrackingService {
public:
    FaceTrackingService(
        std::unique_ptr<VideoSource> video_input,
        std::unique_ptr<FacePipeline> face_pipeline,
        Live2DMappingConfig mapping_config,
        FaceTrackingControlConfig control_config);
    ~FaceTrackingService();

    FaceTrackingService(const FaceTrackingService&) = delete;
    FaceTrackingService& operator=(const FaceTrackingService&) = delete;
    FaceTrackingService(FaceTrackingService&&) = delete;
    FaceTrackingService& operator=(FaceTrackingService&&) = delete;

    void start();
    void request_stop() noexcept;
    void stop();
    void rethrow_if_failed() const;
    std::optional<FaceTrackingSnapshot> take_latest();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vtb
