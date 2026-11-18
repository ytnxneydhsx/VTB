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
    require(config.output.rtsp_enabled, "legacy RTSP output was disabled");
    require(!config.output.webrtc_enabled, "default WebRTC output was enabled");
    require(config.output.rtsp_url == "rtsp://127.0.0.1:8554/live2d", "output URL mismatch");
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

void test_uvc_output_configuration() {
    std::string json = valid_configuration();
    const std::string expected =
        R"json("rtsp_url": "rtsp://127.0.0.1:8554/live2d")json";
    const std::size_t position = json.find(expected);
    require(position != std::string::npos, "output URL is missing");
    json.replace(
        position,
        expected.size(),
        expected + R"json(,
            "uvc": {
                "enabled": true,
                "device": "/dev/video2",
                "link_path": "/run/vtb/uvc-link.mjpeg",
                "gst_launch_path": "/opt/vtb/gstreamer-1.28.9/bin/gst-launch-1.0",
                "library_path": "/opt/vtb/gstreamer-1.28.9/lib/aarch64-linux-gnu",
                "plugin_path": "/opt/vtb/gstreamer-1.28.9/lib/aarch64-linux-gnu/gstreamer-1.0",
                "fps": 30
            })json");

    vtb::VtbApplicationConfigParser parser;
    const vtb::VtbApplicationConfiguration config = parser.parse(json);
    require(config.output.uvc_enabled, "UVC output was not enabled");
    require(config.output.uvc_device == "/dev/video2", "UVC device mismatch");
    require(
        config.output.uvc_link_path == "/run/vtb/uvc-link.mjpeg",
        "UVC link path mismatch");
    require(config.output.uvc_fps == 30, "UVC fps mismatch");
}

void test_uvc_fps_does_not_exceed_render_fps() {
    std::string json = valid_configuration();
    const std::string expected =
        R"json("rtsp_url": "rtsp://127.0.0.1:8554/live2d")json";
    const std::size_t position = json.find(expected);
    require(position != std::string::npos, "output URL is missing");
    json.replace(
        position,
        expected.size(),
        expected + R"json(,
            "uvc": {
                "enabled": true,
                "device": "/dev/video2",
                "gst_launch_path": "/opt/vtb/gstreamer-1.28.9/bin/gst-launch-1.0",
                "library_path": "/opt/vtb/gstreamer-1.28.9/lib/aarch64-linux-gnu",
                "plugin_path": "/opt/vtb/gstreamer-1.28.9/lib/aarch64-linux-gnu/gstreamer-1.0",
                "fps": 60
            })json");

    vtb::VtbApplicationConfigParser parser;
    bool rejected = false;
    try {
        parser.parse(json);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "UVC fps above render fps was accepted");
}

void test_independent_network_outputs() {
    const auto parse_outputs = [](const std::string& fields) {
        std::string json = valid_configuration();
        const std::string marker =
            R"json("rtsp_url": "rtsp://127.0.0.1:8554/live2d")json";
        const auto position = json.find(marker);
        require(position != std::string::npos, "output URL is missing");
        json.replace(position, marker.size(), fields);
        return vtb::VtbApplicationConfigParser{}.parse(json).output;
    };

    const auto disabled = parse_outputs(R"json("rtsp_enabled": false)json");
    require(!disabled.rtsp_enabled, "RTSP output was enabled");
    require(!disabled.webrtc_enabled, "WebRTC output was enabled");

    const auto webrtc_only = parse_outputs(R"json(
        "rtsp_enabled": false,
        "webrtc": {
            "enabled": true,
            "bind_address": "192.168.77.2",
            "web_directory": "/runtime/webrtc"
        },
        "gstreamer_plugin_path": "/runtime/gstreamer")json");
    require(!webrtc_only.rtsp_enabled && webrtc_only.rtsp_url.empty(),
            "WebRTC-only mode depends on RTSP");
    require(webrtc_only.webrtc_enabled, "WebRTC output was disabled");
    require(webrtc_only.webrtc_bind_address == "192.168.77.2",
            "WebRTC bind address mismatch");
    require(webrtc_only.webrtc_web_directory == "/runtime/webrtc",
            "WebRTC web directory mismatch");
    require(webrtc_only.gstreamer_plugin_path == "/runtime/gstreamer",
            "standalone GStreamer plugin directory mismatch");

    const auto both = parse_outputs(R"json(
        "rtsp_url": "rtsp://127.0.0.1:8554/live2d",
        "webrtc": {"enabled": true, "web_directory": "/runtime/webrtc"})json");
    require(both.rtsp_enabled && both.webrtc_enabled,
            "legacy dual-output configuration was rejected");
    require(both.webrtc_bind_address.empty(),
            "legacy configuration should defer WebRTC address resolution");

    bool rejected = false;
    try {
        parse_outputs(R"json(
            "rtsp_enabled": false,
            "webrtc": {"enabled": true, "web_directory": "/runtime/webrtc"})json");
    } catch (const std::invalid_argument& error) {
        rejected = std::string(error.what()).find("output.webrtc.bind_address") !=
            std::string::npos;
    }
    require(rejected, "WebRTC-only mode accepted a missing bind address");
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
        test_uvc_output_configuration();
        test_uvc_fps_does_not_exceed_render_fps();
        test_independent_network_outputs();
        std::cout << "vtb_application_config_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "vtb_application_config_test failed: "
                  << error.what() << "\n";
        return 1;
    }
}
