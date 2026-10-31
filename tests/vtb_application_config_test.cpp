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
            "pixel_format": "MJPG",
            "width": 1280,
            "height": 720,
            "fps": 60,
            "buffer_count": 4
        },
        "face_pipeline": {
            "detector_model": "/models/detector.rknn",
            "landmark_model": "/models/landmark.rknn",
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
            "rtsp_url": "rtsp://127.0.0.1:8554/live2d",
            "output_width": 1920,
            "output_height": 1080,
            "output_fps": 30,
            "total_frames": 0
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
    require(config.video_input.pixel_format == "MJPG", "format mismatch");
    require(config.video_input.width == 1280, "width mismatch");
    require(config.face_pipeline.minimum_tracking_confidence == 0.6F,
            "tracking confidence mismatch");
    require(config.control.port == 11575, "control port mismatch");
    require(config.render.output_fps == 30, "output fps mismatch");
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

}  // namespace

int main() {
    try {
        test_valid_configuration();
        test_unsupported_input_type();
        std::cout << "vtb_application_config_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "vtb_application_config_test failed: "
                  << error.what() << "\n";
        return 1;
    }
}
