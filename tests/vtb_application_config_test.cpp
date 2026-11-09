#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

#include "vtb/config/vtb_application_config.hpp"

namespace {

const char* valid_configuration() {
    return R"json({
        "video_input": {
            "type": "v4l2_camera",
            "device": "/dev/video-test",
            "pixel_format": "YUYV",
            "width": 640,
            "height": 480,
            "fps": 30,
            "buffer_count": 4
        },
        "face_pipeline": {
            "detector_model": "/models/detector.rknn",
            "landmark_model": "/models/landmark.rknn",
            "rga_dma_heap": "/dev/dma_heap/system-uncached",
            "redetect_interval_frames": 0,
            "minimum_tracking_confidence": 0.6
        },
        "mapping": {
            "calibration_frames": 30,
            "smoothing_tau_ms": 50.0,
            "angle_x_sign": -1.0,
            "angle_y_sign": -1.0,
            "angle_z_sign": -1.0
        },
        "control": {
            "bind_address": "192.168.77.2",
            "port": 11575,
            "allowed_origin": "http://192.168.77.1:8900"
        },
        "render": {
            "godot_project_dir": "/runtime/project",
            "live2d_model": "/models/live2d/model3.json",
            "output_width": 1920,
            "output_height": 1080,
            "output_fps": 30,
            "total_frames": 0
        },
        "output": {
            "rtsp_url": "rtsp://127.0.0.1:8554/live2d"
        },
        "face_diagnostic_output": {
            "enabled": true,
            "rtsp_url": "rtsp://127.0.0.1:8554/face3d",
            "maximum_fps": 30,
            "bitrate": 1200000,
            "gop": 30
        },
        "instrumentation": {
            "metrics_directory": "/runtime/metrics",
            "report_interval_ms": 5000
        }
    })json";
}

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_valid_configuration() {
    vtb::VtbApplicationConfigParser parser;
    const vtb::VtbApplicationConfiguration config =
        parser.parse(valid_configuration());
    require(
        config.video_input.type == vtb::VideoInputType::v4l2_camera,
        "video input type mismatch");
    require(config.video_input.device == "/dev/video-test", "device mismatch");
    require(config.video_input.pixel_format == "YUYV", "format mismatch");
    require(config.video_input.width == 640, "width mismatch");
    require(config.face_pipeline.minimum_tracking_confidence == 0.6F,
            "tracking confidence mismatch");
    require(
        config.face_pipeline.rga_dma_heap ==
            "/dev/dma_heap/system-uncached",
        "RGA DMA heap mismatch");
    require(config.control.port == 11575, "control port mismatch");
    require(config.render.output_fps == 30, "output fps mismatch");
    require(config.output.rtsp_url == "rtsp://127.0.0.1:8554/live2d", "output URL mismatch");
    require(
        config.face_diagnostic_output.enabled,
        "face diagnostic output was not enabled");
    require(
        config.face_diagnostic_output.rtsp_url ==
            "rtsp://127.0.0.1:8554/face3d",
        "face diagnostic output URL mismatch");
    require(
        config.face_diagnostic_output.maximum_fps == 30,
        "face diagnostic maximum FPS mismatch");
    require(
        config.instrumentation.metrics_persistence_enabled(),
        "metrics persistence was not enabled");
    require(
        config.instrumentation.metrics_directory == "/runtime/metrics",
        "metrics directory mismatch");
    require(
        config.instrumentation.report_interval_ms == 5000,
        "metrics report interval mismatch");
}

void test_unsupported_input_type() {
    std::string json = valid_configuration();
    const std::string expected = "v4l2_camera";
    const std::size_t position = json.find(expected);
    require(position != std::string::npos, "test JSON type is missing");
    json.replace(position, expected.size(), "network_stream");
    vtb::VtbApplicationConfigParser parser;
    bool rejected = false;
    try {
        parser.parse(json);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "unsupported video input type was accepted");
}

void test_compressed_camera_format_requires_decoder() {
    std::string json = valid_configuration();
    const std::string expected = "YUYV";
    const std::size_t position = json.find(expected);
    require(position != std::string::npos, "test JSON format is missing");
    json.replace(position, expected.size(), "MJPG");
    vtb::VtbApplicationConfigParser parser;
    bool rejected = false;
    try {
        parser.parse(json);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "compressed camera format was accepted without a decoder");
}

void test_render_output_url_requires_migration() {
    std::string json = valid_configuration();
    const std::string marker = "\"godot_project_dir\"";
    const auto position = json.find(marker);
    require(position != std::string::npos, "render section is missing");
    json.insert(position, "\"rtsp_url\":\"rtsp://old-config/live2d\",");
    vtb::VtbApplicationConfigParser parser;
    bool rejected = false;
    try {
        parser.parse(json);
    } catch (const std::invalid_argument& error) {
        rejected = std::string(error.what()).find("output.rtsp_url") != std::string::npos;
    }
    require(rejected, "legacy render output URL lacked a migration error");
}

void test_instrumentation_section_is_optional() {
    std::string json = valid_configuration();
    const std::string section = R"json(,
        "instrumentation": {
            "metrics_directory": "/runtime/metrics",
            "report_interval_ms": 5000
        })json";
    const std::size_t position = json.find(section);
    require(position != std::string::npos, "instrumentation section is missing");
    json.erase(position, section.size());

    vtb::VtbApplicationConfigParser parser;
    const vtb::VtbApplicationConfiguration config = parser.parse(json);
    require(
        !config.instrumentation.metrics_persistence_enabled(),
        "missing instrumentation section enabled persistence");
    require(
        config.instrumentation.report_interval_ms == 60000,
        "default metrics interval mismatch");
}

void test_instrumentation_interval_range() {
    std::string json = valid_configuration();
    const std::string expected = "\"report_interval_ms\": 5000";
    const std::size_t position = json.find(expected);
    require(position != std::string::npos, "metrics interval is missing");
    json.replace(position, expected.size(), "\"report_interval_ms\": 999");

    vtb::VtbApplicationConfigParser parser;
    bool rejected = false;
    try {
        parser.parse(json);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "sub-second metrics interval was accepted");
}

void test_face_diagnostic_section_is_optional() {
    std::string json = valid_configuration();
    const std::string section = R"json(,
        "face_diagnostic_output": {
            "enabled": true,
            "rtsp_url": "rtsp://127.0.0.1:8554/face3d",
            "maximum_fps": 30,
            "bitrate": 1200000,
            "gop": 30
        })json";
    const std::size_t position = json.find(section);
    require(position != std::string::npos, "face diagnostic section is missing");
    json.erase(position, section.size());

    vtb::VtbApplicationConfigParser parser;
    const vtb::VtbApplicationConfiguration config = parser.parse(json);
    require(
        !config.face_diagnostic_output.enabled,
        "missing face diagnostic section enabled output");
}

void test_disabled_face_diagnostic_requires_no_output_fields() {
    std::string json = valid_configuration();
    const std::string section = R"json("face_diagnostic_output": {
            "enabled": true,
            "rtsp_url": "rtsp://127.0.0.1:8554/face3d",
            "maximum_fps": 30,
            "bitrate": 1200000,
            "gop": 30
        })json";
    const std::size_t position = json.find(section);
    require(position != std::string::npos, "face diagnostic section is missing");
    json.replace(
        position,
        section.size(),
        R"json("face_diagnostic_output": {"enabled": false})json");

    vtb::VtbApplicationConfigParser parser;
    const vtb::VtbApplicationConfiguration config = parser.parse(json);
    require(
        !config.face_diagnostic_output.enabled,
        "disabled face diagnostic output was enabled");
}

void test_face_diagnostic_fps_must_not_exceed_input() {
    std::string json = valid_configuration();
    const std::string expected = "\"maximum_fps\": 30";
    const std::size_t position = json.find(expected);
    require(position != std::string::npos, "face diagnostic FPS is missing");
    json.replace(position, expected.size(), "\"maximum_fps\": 31");

    vtb::VtbApplicationConfigParser parser;
    bool rejected = false;
    try {
        parser.parse(json);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "face diagnostic FPS above input FPS was accepted");
}

}  // namespace

int main() {
    try {
        test_valid_configuration();
        test_unsupported_input_type();
        test_compressed_camera_format_requires_decoder();
        test_render_output_url_requires_migration();
        test_instrumentation_section_is_optional();
        test_instrumentation_interval_range();
        test_face_diagnostic_section_is_optional();
        test_disabled_face_diagnostic_requires_no_output_fields();
        test_face_diagnostic_fps_must_not_exceed_input();
        std::cout << "vtb_application_config_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "vtb_application_config_test failed: "
                  << error.what() << "\n";
        return 1;
    }
}
