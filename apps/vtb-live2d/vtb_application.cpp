#include "vtb_application.hpp"

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/extension/libgodot.h"
#include "openvt_libgodot_bridge.h"
#include "vtb/face/cpu_image_transformer.hpp"
#include "vtb/face/face_tracking_service.hpp"
#include "vtb/face/openseeface_geometry_estimator.hpp"
#include "vtb/face/rknn_face_models.hpp"
#include "vtb/video/opencv_video_source.hpp"

namespace vtb {
namespace {

void initialize_host_extension(void*, GDExtensionInitializationLevel) {}

void deinitialize_host_extension(void*, GDExtensionInitializationLevel) {}

GDExtensionBool host_extension_init(
    GDExtensionInterfaceGetProcAddress,
    GDExtensionClassLibraryPtr,
    GDExtensionInitialization* initialization) {
    if (initialization == nullptr) {
        return 0;
    }
    initialization->minimum_initialization_level =
        GDEXTENSION_INITIALIZATION_CORE;
    initialization->userdata = nullptr;
    initialization->initialize = initialize_host_extension;
    initialization->deinitialize = deinitialize_host_extension;
    return 1;
}

std::string required_environment(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        throw std::invalid_argument(std::string(name) + " is required");
    }
    return value;
}

std::string optional_environment(const char* name, const char* default_value) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return default_value;
    }
    return value;
}

int integer_environment(
    const char* name,
    int default_value,
    int minimum,
    int maximum) {
    const char* text = std::getenv(name);
    if (text == nullptr || text[0] == '\0') {
        return default_value;
    }
    errno = 0;
    char* end = nullptr;
    const long parsed = std::strtol(text, &end, 10);
    if (errno == ERANGE || end == text || end == nullptr || end[0] != '\0' ||
        parsed < minimum || parsed > maximum) {
        throw std::invalid_argument(
            std::string(name) + " is outside the supported range");
    }
    return static_cast<int>(parsed);
}

float floating_environment(const char* name, float default_value) {
    const char* text = std::getenv(name);
    if (text == nullptr || text[0] == '\0') {
        return default_value;
    }
    errno = 0;
    char* end = nullptr;
    const float parsed = std::strtof(text, &end);
    if (errno == ERANGE || end == text || end == nullptr || end[0] != '\0' ||
        !std::isfinite(parsed)) {
        throw std::invalid_argument(
            std::string(name) + " must be a finite number");
    }
    return parsed;
}

class ApplicationArguments final {
public:
    ApplicationArguments(int argc, char** argv) {
        if (argc < 1 || argv == nullptr || argv[0] == nullptr) {
            throw std::invalid_argument("Invalid process arguments");
        }
        godot_arguments_.emplace_back(argv[0]);
        for (int index = 1; index < argc; ++index) {
            const std::string argument(argv[index]);
            if (argument == "--vtb-iterations") {
                if (index + 1 >= argc) {
                    throw std::invalid_argument(
                        "--vtb-iterations requires a value");
                }
                iteration_limit_ = parse_positive_integer(argv[++index]);
                continue;
            }
            godot_arguments_.push_back(argument);
        }
        if (!has_argument("--path")) {
            godot_arguments_.emplace_back("--path");
            godot_arguments_.push_back(
                required_environment("OPENVT_GODOT_PROJECT_DIR"));
        }
        append_default_argument("--display-driver", "x11");
        append_default_argument("--rendering-driver", "opengl3_es");
        append_default_flag("--single-window");
        append_default_flag("--disable-crash-handler");
        append_default_flag("--disable-vsync");
        output_fps_ = integer_environment(
            "OPENVT_OUTPUT_FPS", 30, 1, 1000);
        if (!has_argument("--fixed-fps")) {
            godot_arguments_.emplace_back("--fixed-fps");
            godot_arguments_.push_back(std::to_string(output_fps_));
        }
        refresh_pointers();
    }

    ~ApplicationArguments() = default;

    ApplicationArguments(const ApplicationArguments&) = delete;
    ApplicationArguments& operator=(const ApplicationArguments&) = delete;
    ApplicationArguments(ApplicationArguments&&) = delete;
    ApplicationArguments& operator=(ApplicationArguments&&) = delete;

    int godot_argument_count() const noexcept {
        return static_cast<int>(godot_argument_pointers_.size());
    }

    char** godot_arguments() noexcept {
        return godot_argument_pointers_.data();
    }

    unsigned long iteration_limit() const noexcept {
        return iteration_limit_;
    }

    std::chrono::nanoseconds frame_period() const noexcept {
        return std::chrono::nanoseconds(
            1000000000ULL / static_cast<unsigned long>(output_fps_));
    }

private:
    static unsigned long parse_positive_integer(const char* text) {
        if (text == nullptr || text[0] == '\0') {
            throw std::invalid_argument("Iteration limit is empty");
        }
        errno = 0;
        char* end = nullptr;
        const unsigned long value = std::strtoul(text, &end, 10);
        if (errno == ERANGE || end == text || end == nullptr ||
            end[0] != '\0' || value == 0) {
            throw std::invalid_argument(
                "Iteration limit must be a positive integer");
        }
        return value;
    }

    bool has_argument(const std::string& name) const {
        for (const std::string& argument : godot_arguments_) {
            if (argument == name) {
                return true;
            }
        }
        return false;
    }

    void append_default_argument(
        const std::string& name,
        const std::string& value) {
        if (!has_argument(name)) {
            godot_arguments_.push_back(name);
            godot_arguments_.push_back(value);
        }
    }

    void append_default_flag(const std::string& name) {
        if (!has_argument(name)) {
            godot_arguments_.push_back(name);
        }
    }

    void refresh_pointers() {
        godot_argument_pointers_.clear();
        godot_argument_pointers_.reserve(godot_arguments_.size());
        for (std::string& argument : godot_arguments_) {
            godot_argument_pointers_.push_back(argument.data());
        }
    }

    std::vector<std::string> godot_arguments_;
    std::vector<char*> godot_argument_pointers_;
    unsigned long iteration_limit_ = 0;
    int output_fps_ = 30;
};

class FaceModuleConfig final {
public:
    FaceModuleConfig()
        : source(required_environment("OPENVT_FACE_SOURCE")),
          detector_model(required_environment("OPENVT_FACE_DETECTOR_MODEL")),
          landmark_model(required_environment("OPENVT_FACE_LANDMARK_MODEL")),
          width(integer_environment("OPENVT_FACE_WIDTH", 1280, 1, 8192)),
          height(integer_environment("OPENVT_FACE_HEIGHT", 720, 1, 8192)),
          fps(integer_environment("OPENVT_FACE_FPS", 60, 1, 240)),
          buffer_count(integer_environment("OPENVT_FACE_BUFFERS", 4, 2, 64)),
          calibration_frames(integer_environment(
              "OPENVT_FACE_CALIBRATION_FRAMES", 30, 1, 10000)),
          smoothing_tau_ms(floating_environment(
              "OPENVT_FACE_SMOOTHING_MS", 50.0F)),
          angle_x_sign(floating_environment(
              "OPENVT_FACE_ANGLE_X_SIGN", -1.0F)),
          angle_y_sign(floating_environment(
              "OPENVT_FACE_ANGLE_Y_SIGN", -1.0F)),
          angle_z_sign(floating_environment(
              "OPENVT_FACE_ANGLE_Z_SIGN", -1.0F)),
          control_address(optional_environment(
              "OPENVT_LIVE2D_CONTROL_ADDRESS", "192.168.77.2")),
          control_port(integer_environment(
              "OPENVT_LIVE2D_CONTROL_PORT", 11575, 1, 65535)),
          control_origin(optional_environment(
              "OPENVT_LIVE2D_CONTROL_ORIGIN",
              "http://192.168.77.1:8900")) {}

    ~FaceModuleConfig() = default;

    FaceModuleConfig(const FaceModuleConfig&) = default;
    FaceModuleConfig& operator=(const FaceModuleConfig&) = default;
    FaceModuleConfig(FaceModuleConfig&&) noexcept = default;
    FaceModuleConfig& operator=(FaceModuleConfig&&) noexcept = default;

    std::string source;
    std::string detector_model;
    std::string landmark_model;
    int width = 1280;
    int height = 720;
    int fps = 60;
    int buffer_count = 4;
    int calibration_frames = 30;
    float smoothing_tau_ms = 50.0F;
    float angle_x_sign = -1.0F;
    float angle_y_sign = -1.0F;
    float angle_z_sign = -1.0F;
    std::string control_address;
    int control_port = 11575;
    std::string control_origin;
};

class LibGodotInstance final {
public:
    explicit LibGodotInstance(ApplicationArguments& arguments) {
        instance_ = libgodot_create_godot_instance(
            arguments.godot_argument_count(),
            arguments.godot_arguments(),
            host_extension_init);
        if (instance_ == nullptr) {
            throw std::runtime_error("LibGodot instance creation failed");
        }
    }

    ~LibGodotInstance() {
        shutdown();
    }

    LibGodotInstance(const LibGodotInstance&) = delete;
    LibGodotInstance& operator=(const LibGodotInstance&) = delete;
    LibGodotInstance(LibGodotInstance&&) = delete;
    LibGodotInstance& operator=(LibGodotInstance&&) = delete;

    void start() {
        const int result = openvt_libgodot_start(instance_);
        if (result != 0) {
            throw std::runtime_error(
                "LibGodot start failed: " + std::to_string(result));
        }
        const int application_result =
            openvt_libgodot_application_initialize();
        if (application_result != 0) {
            throw_application_error(
                "C++ Live2D application initialization failed",
                application_result);
        }
        application_initialized_ = true;
    }

    void submit(const FaceTrackingSnapshot& snapshot) {
        const Live2DParameters& parameters = snapshot.mapping().parameters();
        OpenVTLive2DFrameInput input{};
        input.angle_x = parameters.angle_x();
        input.angle_y = parameters.angle_y();
        input.angle_z = parameters.angle_z();
        input.body_angle_x = parameters.body_angle_x();
        input.body_angle_y = parameters.body_angle_y();
        input.body_angle_z = parameters.body_angle_z();
        input.left_eye_open = parameters.left_eye_open();
        input.right_eye_open = parameters.right_eye_open();
        input.mouth_open = parameters.mouth_open();
        input.mouth_form = parameters.mouth_form();
        input.view_zoom = snapshot.view().zoom();
        input.view_offset_x = snapshot.view().offset_x();
        input.view_offset_y = snapshot.view().offset_y();
        input.produced_timestamp_ns =
            snapshot.mapping().produced_timestamp_ns();
        input.generation = snapshot.generation();
        const int result =
            openvt_libgodot_application_submit_live2d_frame(&input);
        if (result != 0) {
            throw_application_error(
                "Live2D parameter submission failed", result);
        }
    }

    bool run_frame() {
        const int prepare_result =
            openvt_libgodot_application_prepare_frame(monotonic_now_ns());
        if (prepare_result != 0) {
            throw_application_error(
                "C++ Live2D frame preparation failed", prepare_result);
        }
        const int result = openvt_libgodot_iteration(instance_);
        if (result < 0) {
            throw std::runtime_error(
                "LibGodot iteration failed: " + std::to_string(result));
        }
        const int complete_result =
            openvt_libgodot_application_complete_frame();
        if (complete_result != 0) {
            throw_application_error(
                "C++ Live2D frame completion failed", complete_result);
        }
        return result > 0 ||
            openvt_libgodot_application_should_stop() != 0;
    }

    std::uint64_t completed_frames() const noexcept {
        return openvt_libgodot_application_completed_frames();
    }

    int shutdown() noexcept {
        int application_result = 0;
        if (application_initialized_) {
            application_result = openvt_libgodot_application_shutdown();
            application_initialized_ = false;
        }
        if (instance_ != nullptr) {
            libgodot_destroy_godot_instance(instance_);
            instance_ = nullptr;
        }
        return application_result;
    }

private:
    static std::uint64_t monotonic_now_ns() noexcept {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    }

    [[noreturn]] static void throw_application_error(
        const std::string& message,
        int error_code) {
        throw std::runtime_error(
            message + ": " + std::to_string(error_code) + " " +
            openvt_libgodot_application_last_error());
    }

    GDExtensionObjectPtr instance_ = nullptr;
    bool application_initialized_ = false;
};

std::unique_ptr<VideoSource> create_video_input(
    const FaceModuleConfig& config) {
    OpenCvVideoSourceConfig source_config;
    source_config.source = config.source;
    source_config.width = static_cast<std::uint32_t>(config.width);
    source_config.height = static_cast<std::uint32_t>(config.height);
    source_config.fps = static_cast<std::uint32_t>(config.fps);
    source_config.buffer_count =
        static_cast<std::uint32_t>(config.buffer_count);
    return std::make_unique<OpenCvVideoSource>(std::move(source_config));
}

std::unique_ptr<FacePipeline> create_face_pipeline(
    const FaceModuleConfig& config) {
    auto image_transformer = std::make_unique<CpuImageTransformer>();
    auto detector = std::make_unique<RknnFaceDetector>(
        config.detector_model, RknnCore::core0);
    auto landmark = std::make_unique<RknnLandmarkEstimator>(
        config.landmark_model, RknnCore::core1);
    auto geometry = std::make_unique<OpenSeeFaceGeometryEstimator>();
    FacePipelineConfig face_config;
    face_config.redetect_interval_frames = 0;
    face_config.minimum_tracking_confidence = 0.6F;
    return std::make_unique<FacePipeline>(
        face_config,
        std::move(image_transformer),
        std::move(detector),
        std::move(landmark),
        std::move(geometry));
}

std::unique_ptr<FaceTrackingService> create_face_tracking_service(
    const FaceModuleConfig& config,
    std::unique_ptr<VideoSource> video_input,
    std::unique_ptr<FacePipeline> face_pipeline) {
    Live2DMappingConfig mapping_config;
    mapping_config.calibration_frames =
        static_cast<std::size_t>(config.calibration_frames);
    mapping_config.smoothing_tau_ms = config.smoothing_tau_ms;
    mapping_config.angle_x_sign = config.angle_x_sign;
    mapping_config.angle_y_sign = config.angle_y_sign;
    mapping_config.angle_z_sign = config.angle_z_sign;

    FaceTrackingControlConfig control_config;
    control_config.bind_address = config.control_address;
    control_config.port = config.control_port;
    control_config.allowed_origin = config.control_origin;

    return std::make_unique<FaceTrackingService>(
        std::move(video_input),
        std::move(face_pipeline),
        mapping_config,
        std::move(control_config));
}

}  // namespace

class VtbApplication::Impl final {
public:
    Impl(int argc, char** argv)
        : arguments_(argc, argv), face_config_() {}

    ~Impl() = default;

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    int run(const std::atomic<bool>& stop_requested) {
        std::unique_ptr<VideoSource> video_input =
            create_video_input(face_config_);
        std::unique_ptr<FacePipeline> face_pipeline =
            create_face_pipeline(face_config_);
        std::unique_ptr<FaceTrackingService> face_tracking =
            create_face_tracking_service(
                face_config_,
                std::move(video_input),
                std::move(face_pipeline));

        LibGodotInstance godot(arguments_);
        godot.start();
        face_tracking->start();

        unsigned long iterations = 0;
        auto next_frame_deadline = std::chrono::steady_clock::now();
        while (!stop_requested.load(std::memory_order_acquire)) {
            std::optional<FaceTrackingSnapshot> snapshot =
                face_tracking->take_latest();
            if (snapshot.has_value()) {
                godot.submit(snapshot.value());
            }
            const bool finished = godot.run_frame();
            ++iterations;
            if (finished) {
                break;
            }
            if (arguments_.iteration_limit() > 0 &&
                iterations >= arguments_.iteration_limit()) {
                break;
            }
            next_frame_deadline += arguments_.frame_period();
            const auto current_time = std::chrono::steady_clock::now();
            if (next_frame_deadline < current_time) {
                next_frame_deadline = current_time;
            }
            std::this_thread::sleep_until(next_frame_deadline);
        }

        face_tracking->request_stop();
        face_tracking->stop();
        const std::uint64_t completed_frames = godot.completed_frames();
        std::cout << "VTB_LIBGODOT_RESULT iterations=" << iterations
                  << " completed_frames=" << completed_frames << "\n";
        const int shutdown_result = godot.shutdown();
        if (shutdown_result != 0) {
            throw std::runtime_error(
                "C++ Live2D application shutdown failed: " +
                std::to_string(shutdown_result) + " " +
                openvt_libgodot_application_last_error());
        }
        return 0;
    }

private:
    ApplicationArguments arguments_;
    FaceModuleConfig face_config_;
};

VtbApplication::VtbApplication(int argc, char** argv)
    : impl_(std::make_unique<Impl>(argc, argv)) {}

VtbApplication::~VtbApplication() = default;

int VtbApplication::run(const std::atomic<bool>& stop_requested) {
    return impl_->run(stop_requested);
}

}  // namespace vtb
