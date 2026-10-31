#include "vtb_application.hpp"

#include <chrono>
#include <cstdint>
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
#include "vtb/config/config_file_reader.hpp"
#include "vtb/config/vtb_application_config.hpp"
#include "vtb/face/cpu_image_transformer.hpp"
#include "vtb/face/face_tracking_service.hpp"
#include "vtb/face/openseeface_geometry_estimator.hpp"
#include "vtb/face/rknn_face_models.hpp"
#include "vtb/video/video_input_factory.hpp"

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

class CommandLineArguments final {
public:
    CommandLineArguments(int argc, char** argv) {
        if (argc < 1 || argv == nullptr || argv[0] == nullptr) {
            throw std::invalid_argument("Invalid process arguments");
        }
        passthrough_arguments_.emplace_back(argv[0]);
        for (int index = 1; index < argc; ++index) {
            const std::string argument(argv[index]);
            if (argument == "--config") {
                if (index + 1 >= argc) {
                    throw std::invalid_argument("--config requires a value");
                }
                if (!config_path_.empty()) {
                    throw std::invalid_argument("--config may only be specified once");
                }
                config_path_ = argv[++index];
                continue;
            }
            if (argument == "--vtb-iterations") {
                if (index + 1 >= argc) {
                    throw std::invalid_argument(
                        "--vtb-iterations requires a value");
                }
                iteration_limit_ = parse_positive_integer(argv[++index]);
                continue;
            }
            passthrough_arguments_.push_back(argument);
        }
        if (config_path_.empty()) {
            throw std::invalid_argument("--config is required");
        }
    }

    ~CommandLineArguments() = default;

    CommandLineArguments(const CommandLineArguments&) = delete;
    CommandLineArguments& operator=(const CommandLineArguments&) = delete;
    CommandLineArguments(CommandLineArguments&&) = delete;
    CommandLineArguments& operator=(CommandLineArguments&&) = delete;

    const std::string& config_path() const noexcept {
        return config_path_;
    }

    const std::vector<std::string>& passthrough_arguments() const noexcept {
        return passthrough_arguments_;
    }

    unsigned long iteration_limit() const noexcept {
        return iteration_limit_;
    }

private:
    static unsigned long parse_positive_integer(const char* text) {
        if (text == nullptr || text[0] == '\0' || text[0] == '-') {
            throw std::invalid_argument(
                "Iteration limit must be a positive integer");
        }
        std::size_t consumed = 0;
        unsigned long value = 0;
        try {
            value = std::stoul(text, &consumed, 10);
        } catch (const std::exception&) {
            throw std::invalid_argument(
                "Iteration limit must be a positive integer");
        }
        if (consumed != std::string(text).size() || value == 0) {
            throw std::invalid_argument(
                "Iteration limit must be a positive integer");
        }
        return value;
    }

    std::string config_path_;
    std::vector<std::string> passthrough_arguments_;
    unsigned long iteration_limit_ = 0;
};

VtbApplicationConfiguration load_application_configuration(
    const std::string& path) {
    ConfigurationFileReader reader;
    VtbApplicationConfigParser parser;
    return parser.parse(reader.read(path));
}

class ApplicationArguments final {
public:
    ApplicationArguments(
        const CommandLineArguments& command_line,
        const RenderConfiguration& render)
        : godot_arguments_(command_line.passthrough_arguments()),
          iteration_limit_(command_line.iteration_limit()),
          output_fps_(render.output_fps) {
        if (has_argument("--path") || has_argument("--fixed-fps")) {
            throw std::invalid_argument(
                "--path and --fixed-fps are managed by the JSON configuration");
        }
        godot_arguments_.emplace_back("--path");
        godot_arguments_.push_back(render.godot_project_dir);
        append_default_argument("--display-driver", "x11");
        append_default_argument("--rendering-driver", "opengl3_es");
        append_default_flag("--single-window");
        append_default_flag("--disable-crash-handler");
        append_default_flag("--disable-vsync");
        godot_arguments_.emplace_back("--fixed-fps");
        godot_arguments_.push_back(std::to_string(output_fps_));
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
    std::uint32_t output_fps_ = 0;
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

    void start(const RenderConfiguration& config) {
        const int result = openvt_libgodot_start(instance_);
        if (result != 0) {
            throw std::runtime_error(
                "LibGodot start failed: " + std::to_string(result));
        }
        OpenVTEmbeddedApplicationConfig embedded_config{};
        embedded_config.live2d_model = config.live2d_model.c_str();
        embedded_config.rtsp_url = config.rtsp_url.c_str();
        embedded_config.output_width = config.output_width;
        embedded_config.output_height = config.output_height;
        embedded_config.output_fps = config.output_fps;
        embedded_config.total_frames = config.total_frames;
        const int application_result =
            openvt_libgodot_application_initialize(&embedded_config);
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

std::unique_ptr<FacePipeline> create_face_pipeline(
    const FacePipelineConfiguration& config) {
    auto image_transformer = std::make_unique<CpuImageTransformer>();
    auto detector = std::make_unique<RknnFaceDetector>(
        config.detector_model, RknnCore::core0);
    auto landmark = std::make_unique<RknnLandmarkEstimator>(
        config.landmark_model, RknnCore::core1);
    auto geometry = std::make_unique<OpenSeeFaceGeometryEstimator>();
    FacePipelineConfig face_config;
    face_config.redetect_interval_frames = config.redetect_interval_frames;
    face_config.minimum_tracking_confidence =
        config.minimum_tracking_confidence;
    return std::make_unique<FacePipeline>(
        face_config,
        std::move(image_transformer),
        std::move(detector),
        std::move(landmark),
        std::move(geometry));
}

std::unique_ptr<FaceTrackingService> create_face_tracking_service(
    const FaceMappingConfiguration& mapping,
    const ControlConfiguration& control,
    std::unique_ptr<VideoSource> video_input,
    std::unique_ptr<FacePipeline> face_pipeline) {
    Live2DMappingConfig mapping_config;
    mapping_config.calibration_frames =
        static_cast<std::size_t>(mapping.calibration_frames);
    mapping_config.smoothing_tau_ms = mapping.smoothing_tau_ms;
    mapping_config.angle_x_sign = mapping.angle_x_sign;
    mapping_config.angle_y_sign = mapping.angle_y_sign;
    mapping_config.angle_z_sign = mapping.angle_z_sign;

    FaceTrackingControlConfig control_config;
    control_config.bind_address = control.bind_address;
    control_config.port = control.port;
    control_config.allowed_origin = control.allowed_origin;

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
        : command_line_(argc, argv),
          config_(load_application_configuration(command_line_.config_path())),
          arguments_(command_line_, config_.render) {}

    ~Impl() = default;

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    int run(const std::atomic<bool>& stop_requested) {
        VideoInputFactory video_input_factory;
        std::unique_ptr<VideoSource> video_input =
            video_input_factory.create(config_.video_input);
        std::unique_ptr<FacePipeline> face_pipeline =
            create_face_pipeline(config_.face_pipeline);
        std::unique_ptr<FaceTrackingService> face_tracking =
            create_face_tracking_service(
                config_.mapping,
                config_.control,
                std::move(video_input),
                std::move(face_pipeline));

        LibGodotInstance godot(arguments_);
        godot.start(config_.render);
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
    CommandLineArguments command_line_;
    VtbApplicationConfiguration config_;
    ApplicationArguments arguments_;
};

VtbApplication::VtbApplication(int argc, char** argv)
    : impl_(std::make_unique<Impl>(argc, argv)) {}

VtbApplication::~VtbApplication() = default;

int VtbApplication::run(const std::atomic<bool>& stop_requested) {
    return impl_->run(stop_requested);
}

}  // namespace vtb
