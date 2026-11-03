#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace vtb {

enum class VideoInputType {
    v4l2_camera,
};

class VideoInputConfiguration {
public:
    VideoInputType type = VideoInputType::v4l2_camera;
    std::string device;
    std::string pixel_format;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t fps = 0;
    std::uint32_t buffer_count = 0;
};

class FacePipelineConfiguration {
public:
    std::string detector_model;
    std::string landmark_model;
    std::string rga_dma_heap;
    std::uint32_t redetect_interval_frames = 0;
    float minimum_tracking_confidence = 0.0F;
};

class FaceMappingConfiguration {
public:
    std::uint32_t calibration_frames = 0;
    float smoothing_tau_ms = 0.0F;
    float angle_x_sign = 0.0F;
    float angle_y_sign = 0.0F;
    float angle_z_sign = 0.0F;
};

class ControlConfiguration {
public:
    std::string bind_address;
    std::uint16_t port = 0;
    std::string allowed_origin;
};

class RenderConfiguration {
public:
    std::string godot_project_dir;
    std::string live2d_model;
    std::uint32_t output_width = 0;
    std::uint32_t output_height = 0;
    std::uint32_t output_fps = 0;
    std::uint32_t total_frames = 0;
};

class MediaOutputConfiguration {
public:
    std::string rtsp_url;
};

class VtbApplicationConfiguration {
public:
    VideoInputConfiguration video_input;
    FacePipelineConfiguration face_pipeline;
    FaceMappingConfiguration mapping;
    ControlConfiguration control;
    RenderConfiguration render;
    MediaOutputConfiguration output;
};

class VtbApplicationConfigParser final {
public:
    VtbApplicationConfiguration parse(std::string_view json_text) const;
};

}  // namespace vtb
