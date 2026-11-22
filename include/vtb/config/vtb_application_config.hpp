#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "vtb/face/face_types.hpp"
#include "vtb/live2d/live2d_face_mapper.hpp"

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

class ControlConfiguration {
public:
    std::string bind_address;
    std::uint16_t port = 0;
    std::string allowed_origin;
};

class RenderConfiguration {
public:
    std::string rga_dma_heap;
    std::string godot_project_dir;
    std::string live2d_model;
    std::uint32_t output_width = 0;
    std::uint32_t output_height = 0;
    std::uint32_t output_fps = 0;
    std::uint32_t total_frames = 0;
};

class MediaOutputConfiguration {
public:
    bool rtsp_enabled = true;
    std::string rtsp_url;
    bool webrtc_enabled = false;
    std::string webrtc_web_directory;
    std::string webrtc_bind_address;
    bool uvc_enabled = false;
    std::string uvc_device;
    std::string gst_launch_path;
    std::string gstreamer_library_path;
    std::string gstreamer_plugin_path;
};

class VtbApplicationConfiguration {
public:
    VideoInputConfiguration video_input;
    FacePipelineConfig face_pipeline;
    Live2DMappingConfig mapping;
    ControlConfiguration control;
    RenderConfiguration render;
    MediaOutputConfiguration output;
};

class VtbApplicationConfigParser final {
public:
    VtbApplicationConfiguration parse(std::string_view json_text) const;
};

}  // namespace vtb
