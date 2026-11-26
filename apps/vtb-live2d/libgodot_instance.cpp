#include "libgodot_instance.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "core/extension/libgodot.h"
#include "openvt_libgodot_bridge.h"

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

class ApplicationArguments final {
public:
    ApplicationArguments(
        const std::vector<std::string>& passthrough_arguments,
        const RenderConfiguration& render)
        : godot_arguments_(passthrough_arguments) {
        if (has_argument("--path") || has_argument("--fixed-fps") ||
            has_argument("--render-thread")) {
            throw std::invalid_argument(
                "--path, --fixed-fps and --render-thread are managed by VTB");
        }
        godot_arguments_.emplace_back("--path");
        godot_arguments_.push_back(render.godot_project_dir);
        append_default_argument("--display-driver", "headless_egl");
        append_default_argument("--rendering-driver", "opengl3_es");
        append_default_flag("--single-window");
        append_default_flag("--disable-crash-handler");
        append_default_flag("--disable-vsync");
        append_default_argument("--render-thread", "safe");
        godot_arguments_.emplace_back("--fixed-fps");
        godot_arguments_.push_back(std::to_string(render.output_fps));
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
};

}  // namespace

class LibGodotInstance::Impl final {
public:
    Impl(
        const std::vector<std::string>& passthrough_arguments,
        RenderConfiguration render_config)
        : render_config_(std::move(render_config)),
          arguments_(passthrough_arguments, render_config_) {
        if (openvt_libgodot_bridge_abi_version() !=
            OPENVT_LIBGODOT_ABI_VERSION) {
            throw std::runtime_error(
                "LibGodot bridge ABI mismatch; rebuild host and library together");
        }
        instance_ = libgodot_create_godot_instance(
            arguments_.godot_argument_count(),
            arguments_.godot_arguments(),
            host_extension_init);
        if (instance_ == nullptr) {
            throw std::runtime_error("LibGodot instance creation failed");
        }
    }

    ~Impl() {
        shutdown();
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void start() {
        if (started_) {
            throw std::logic_error("LibGodot instance can only be started once");
        }
        const int result = openvt_libgodot_start(instance_);
        if (result != 0) {
            throw std::runtime_error(
                "LibGodot start failed: " + std::to_string(result));
        }
        OpenVTEmbeddedApplicationConfig embedded_config{};
        embedded_config.live2d_model = render_config_.live2d_model.c_str();
        embedded_config.rgba_dma_heap = render_config_.rga_dma_heap.c_str();
        embedded_config.abi_version = OPENVT_LIBGODOT_ABI_VERSION;
        embedded_config.config_size = sizeof(embedded_config);
        embedded_config.rgba_sink = rgba_sink();
        embedded_config.output_width = render_config_.output_width;
        embedded_config.output_height = render_config_.output_height;
        embedded_config.output_fps = render_config_.output_fps;
        embedded_config.total_frames = render_config_.total_frames;
        application_cleanup_required_ = true;
        const int application_result =
            openvt_libgodot_application_initialize_v8(&embedded_config);
        if (application_result != 0) {
            throw_application_error(
                "C++ Live2D application initialization failed",
                application_result);
        }
        started_ = true;
    }

    void submit(const Live2DTrackingSnapshot& snapshot) {
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
        input.snapshot_generation = snapshot.snapshot_generation();
        const int result =
            openvt_libgodot_application_submit_live2d_frame(&input);
        if (result != 0) {
            throw_application_error(
                "Live2D parameter submission failed", result);
        }
    }

    bool prepare_frame(std::uint64_t timestamp_ns) {
        int ready = 0;
        const int result =
            openvt_libgodot_application_prepare_frame_v8(timestamp_ns, &ready);
        if (result != 0) {
            throw_application_error(
                "C++ Live2D frame preparation failed", result);
        }
        return ready != 0;
    }

    bool iterate() {
        const int result = openvt_libgodot_iteration(instance_);
        if (result < 0) {
            throw std::runtime_error(
                "LibGodot iteration failed: " + std::to_string(result));
        }
        return result > 0;
    }

    void wait_gpu_render() {
        call_application_step(
            openvt_libgodot_application_wait_gpu_render(),
            "Direct viewport render completion wait failed");
    }

    RgbaFrameLease publish_rgba_frame(std::uint64_t timestamp_ns) {
        rgba_frame_.reset();
        call_application_step(
            openvt_libgodot_application_publish_rgba_frame(timestamp_ns),
            "RGBA frame publication failed");
        if (!rgba_frame_) {
            throw std::runtime_error(
                "LibGodot did not expose the published RGBA frame");
        }
        RgbaFrameLease frame(std::move(*rgba_frame_));
        rgba_frame_.reset();
        return frame;
    }

    bool should_stop() const noexcept {
        return openvt_libgodot_application_should_stop() != 0;
    }

    void finish() {
        const int application_result = finish_application();
        std::string failure_message;
        if (application_result != 0) {
            failure_message =
                "C++ Live2D application shutdown failed: " +
                std::to_string(application_result) + " " +
                openvt_libgodot_application_last_error();
        }
        destroy_instance();
        if (!failure_message.empty()) {
            throw std::runtime_error(failure_message);
        }
    }

    void shutdown() noexcept {
        finish_application();
        destroy_instance();
    }

private:
    static std::int32_t capture_rgba_frame(
        void* context,
        RgbaFrameLease* frame) noexcept {
        if (context == nullptr || frame == nullptr) {
            return -1;
        }
        auto& self = *static_cast<Impl*>(context);
        self.rgba_frame_.emplace(std::move(*frame));
        return 0;
    }

    RgbaFrameSink rgba_sink() noexcept {
        RgbaFrameSink result;
        result.context = this;
        result.submit = capture_rgba_frame;
        return result;
    }

    int finish_application() noexcept {
        int application_result = 0;
        if (application_cleanup_required_) {
            application_result = openvt_libgodot_application_shutdown();
            application_cleanup_required_ = false;
        }
        return application_result;
    }

    void destroy_instance() noexcept {
        if (instance_ != nullptr) {
            libgodot_destroy_godot_instance(instance_);
            instance_ = nullptr;
        }
    }

    static void call_application_step(
        int result,
        const std::string& message) {
        if (result != 0) {
            throw_application_error(message, result);
        }
    }

    [[noreturn]] static void throw_application_error(
        const std::string& message,
        int error_code) {
        throw std::runtime_error(
            message + ": " + std::to_string(error_code) + " " +
            openvt_libgodot_application_last_error());
    }

    RenderConfiguration render_config_;
    ApplicationArguments arguments_;
    GDExtensionObjectPtr instance_ = nullptr;
    std::optional<RgbaFrameLease> rgba_frame_;
    bool application_cleanup_required_ = false;
    bool started_ = false;
};

LibGodotInstance::LibGodotInstance(
    const std::vector<std::string>& passthrough_arguments,
    RenderConfiguration render_config)
    : impl_(std::make_unique<Impl>(
          passthrough_arguments, std::move(render_config))) {}

LibGodotInstance::~LibGodotInstance() = default;

void LibGodotInstance::start() {
    impl_->start();
}

void LibGodotInstance::submit(const Live2DTrackingSnapshot& snapshot) {
    impl_->submit(snapshot);
}

bool LibGodotInstance::prepare_frame(std::uint64_t timestamp_ns) {
    return impl_->prepare_frame(timestamp_ns);
}

bool LibGodotInstance::iterate() {
    return impl_->iterate();
}

void LibGodotInstance::wait_gpu_render() {
    impl_->wait_gpu_render();
}

RgbaFrameLease LibGodotInstance::publish_rgba_frame(
    std::uint64_t timestamp_ns) {
    return impl_->publish_rgba_frame(timestamp_ns);
}

bool LibGodotInstance::should_stop() const noexcept {
    return impl_->should_stop();
}

void LibGodotInstance::finish() {
    impl_->finish();
}

void LibGodotInstance::shutdown() noexcept {
    impl_->shutdown();
}

}  // namespace vtb
