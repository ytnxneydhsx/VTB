#include "vtb/config/vtb_application_config.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace vtb {
namespace {

using Json = nlohmann::json;

const Json& required_object(
    const Json& parent,
    const char* name,
    const char* context) {
    const auto iterator = parent.find(name);
    if (iterator == parent.end() || !iterator->is_object()) {
        throw std::invalid_argument(
            std::string(context) + "." + name + " must be an object");
    }
    return *iterator;
}

template <typename Value>
Value required_value(
    const Json& object,
    const char* name,
    const char* context) {
    const auto iterator = object.find(name);
    if (iterator == object.end()) {
        throw std::invalid_argument(
            std::string(context) + "." + name + " is required");
    }
    try {
        return iterator->get<Value>();
    } catch (const Json::exception&) {
        throw std::invalid_argument(
            std::string(context) + "." + name + " has an invalid type");
    }
}

std::uint32_t required_uint32(
    const Json& object,
    const char* name,
    const char* context,
    std::uint32_t minimum,
    std::uint32_t maximum) {
    const std::int64_t value =
        required_value<std::int64_t>(object, name, context);
    if (value < static_cast<std::int64_t>(minimum) ||
        value > static_cast<std::int64_t>(maximum)) {
        throw std::invalid_argument(
            std::string(context) + "." + name + " is outside the supported range");
    }
    return static_cast<std::uint32_t>(value);
}

float required_float(
    const Json& object,
    const char* name,
    const char* context) {
    const double value = required_value<double>(object, name, context);
    if (!std::isfinite(value) ||
        value < -static_cast<double>(std::numeric_limits<float>::max()) ||
        value > static_cast<double>(std::numeric_limits<float>::max())) {
        throw std::invalid_argument(
            std::string(context) + "." + name + " must be finite");
    }
    return static_cast<float>(value);
}

std::string required_nonempty_string(
    const Json& object,
    const char* name,
    const char* context) {
    std::string value = required_value<std::string>(object, name, context);
    if (value.empty()) {
        throw std::invalid_argument(
            std::string(context) + "." + name + " must not be empty");
    }
    return value;
}

void validate_sign(float value, const char* name) {
    if (value != -1.0F && value != 1.0F) {
        throw std::invalid_argument(
            std::string("mapping.") + name + " must be -1 or 1");
    }
}

VideoInputType parse_video_input_type(const std::string& value) {
    if (value == "v4l2_camera") {
        return VideoInputType::v4l2_camera;
    }
    throw std::invalid_argument(
        "video_input.type is unsupported: " + value);
}

void validate_direct_v4l2_pixel_format(const std::string& value) {
    if (value == "YUYV" || value == "NV12" || value == "NV21") {
        return;
    }
    throw std::invalid_argument(
        "video_input.pixel_format must be YUYV, NV12 or NV21 for direct "
        "V4L2 capture; compressed formats require a decoder");
}

VideoInputConfiguration parse_video_input(const Json& root) {
    const Json& video = required_object(root, "video_input", "root");
    VideoInputConfiguration config;
    config.type = parse_video_input_type(
        required_nonempty_string(video, "type", "video_input"));
    config.device =
        required_nonempty_string(video, "device", "video_input");
    config.pixel_format =
        required_nonempty_string(video, "pixel_format", "video_input");
    if (config.pixel_format.size() != 4U) {
        throw std::invalid_argument(
            "video_input.pixel_format must contain exactly four characters");
    }
    validate_direct_v4l2_pixel_format(config.pixel_format);
    config.width =
        required_uint32(video, "width", "video_input", 1, 8192);
    config.height =
        required_uint32(video, "height", "video_input", 1, 8192);
    config.fps =
        required_uint32(video, "fps", "video_input", 1, 1000);
    config.buffer_count =
        required_uint32(video, "buffer_count", "video_input", 2, 64);
    return config;
}

FacePipelineConfiguration parse_face_pipeline(const Json& root) {
    const Json& face = required_object(root, "face_pipeline", "root");
    FacePipelineConfiguration config;
    config.detector_model =
        required_nonempty_string(face, "detector_model", "face_pipeline");
    config.landmark_model =
        required_nonempty_string(face, "landmark_model", "face_pipeline");
    config.rga_dma_heap =
        required_nonempty_string(face, "rga_dma_heap", "face_pipeline");
    config.redetect_interval_frames = required_uint32(
        face,
        "redetect_interval_frames",
        "face_pipeline",
        0,
        1000000);
    config.minimum_tracking_confidence =
        required_float(face, "minimum_tracking_confidence", "face_pipeline");
    if (config.minimum_tracking_confidence < 0.0F ||
        config.minimum_tracking_confidence > 1.0F) {
        throw std::invalid_argument(
            "face_pipeline.minimum_tracking_confidence must be within [0, 1]");
    }
    return config;
}

FaceMappingConfiguration parse_mapping(const Json& root) {
    const Json& mapping = required_object(root, "mapping", "root");
    FaceMappingConfiguration config;
    config.calibration_frames = required_uint32(
        mapping, "calibration_frames", "mapping", 1, 10000);
    config.smoothing_tau_ms =
        required_float(mapping, "smoothing_tau_ms", "mapping");
    if (config.smoothing_tau_ms < 0.0F) {
        throw std::invalid_argument(
            "mapping.smoothing_tau_ms must be non-negative");
    }
    config.angle_x_sign =
        required_float(mapping, "angle_x_sign", "mapping");
    config.angle_y_sign =
        required_float(mapping, "angle_y_sign", "mapping");
    config.angle_z_sign =
        required_float(mapping, "angle_z_sign", "mapping");
    validate_sign(config.angle_x_sign, "angle_x_sign");
    validate_sign(config.angle_y_sign, "angle_y_sign");
    validate_sign(config.angle_z_sign, "angle_z_sign");
    return config;
}

ControlConfiguration parse_control(const Json& root) {
    const Json& control = required_object(root, "control", "root");
    ControlConfiguration config;
    config.bind_address =
        required_nonempty_string(control, "bind_address", "control");
    config.port = static_cast<std::uint16_t>(
        required_uint32(control, "port", "control", 1, 65535));
    config.allowed_origin =
        required_nonempty_string(control, "allowed_origin", "control");
    return config;
}

RenderConfiguration parse_render(const Json& root) {
    const Json& render = required_object(root, "render", "root");
    RenderConfiguration config;
    config.godot_project_dir =
        required_nonempty_string(render, "godot_project_dir", "render");
    config.live2d_model =
        required_nonempty_string(render, "live2d_model", "render");
    if (render.contains("rtsp_url")) {
        throw std::invalid_argument("Move render.rtsp_url to output.rtsp_url");
    }
    config.output_width =
        required_uint32(render, "output_width", "render", 2, 8192);
    config.output_height =
        required_uint32(render, "output_height", "render", 2, 8192);
    config.output_fps =
        required_uint32(render, "output_fps", "render", 1, 240);
    config.total_frames = required_uint32(
        render,
        "total_frames",
        "render",
        0,
        static_cast<std::uint32_t>(std::numeric_limits<int>::max()));
    if (config.output_width % 2U != 0U ||
        config.output_height % 2U != 0U) {
        throw std::invalid_argument(
            "render output dimensions must be even for NV12 encoding");
    }
    return config;
}

MediaOutputConfiguration parse_output(const Json& root) {
    const Json& output = required_object(root, "output", "root");
    MediaOutputConfiguration config;
    config.rtsp_url = required_nonempty_string(output, "rtsp_url", "output");
    return config;
}

}  // namespace

VtbApplicationConfiguration VtbApplicationConfigParser::parse(
    std::string_view json_text) const {
    if (json_text.empty()) {
        throw std::invalid_argument("Configuration JSON is empty");
    }

    Json root;
    try {
        root = Json::parse(json_text.begin(), json_text.end());
    } catch (const Json::parse_error& error) {
        throw std::invalid_argument(
            "Configuration JSON parse failed at byte " +
            std::to_string(error.byte) + ": " + error.what());
    }
    if (!root.is_object()) {
        throw std::invalid_argument("Configuration root must be an object");
    }

    VtbApplicationConfiguration config;
    config.video_input = parse_video_input(root);
    config.face_pipeline = parse_face_pipeline(root);
    config.mapping = parse_mapping(root);
    config.control = parse_control(root);
    config.render = parse_render(root);
    config.output = parse_output(root);
    return config;
}

}  // namespace vtb
