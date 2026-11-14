#include "vtb/render/godot_runtime.hpp"

#include <cmath>
#include <exception>
#include <stdexcept>
#include <utility>

namespace vtb {
namespace {

std::optional<std::string> validate_config(const GodotRuntimeConfig& config) {
    if (config.project_root.empty()) {
        return std::string("Godot project root must not be empty");
    }
    if (config.model_path.empty()) {
        return std::string("Live2D model path must not be empty");
    }
    if (config.display_driver.empty()) {
        return std::string("Godot display driver must not be empty");
    }
    if (config.rendering_driver.empty()) {
        return std::string("Godot rendering driver must not be empty");
    }
    if (config.mali_library_path.empty()) {
        return std::string("Mali library path must not be empty");
    }
    if (config.output_width == 0 || config.output_height == 0) {
        return std::string("Render output dimensions must be positive");
    }
    if (config.output_fps == 0) {
        return std::string("Render output FPS must be positive");
    }
    if (config.maximum_parameter_age_ns == 0) {
        return std::string("Maximum parameter age must be positive");
    }
    return std::nullopt;
}

std::optional<std::string> validate_parameter_frame(
    const Live2DParameterFrame& frame) {
    if (frame.parameter_generation == 0) {
        return std::string("Parameter generation must be positive");
    }
    if (!std::isfinite(frame.confidence) || frame.confidence < 0.0F ||
        frame.confidence > 1.0F) {
        return std::string("Tracking confidence must be within [0, 1]");
    }
    if (!std::isfinite(frame.view.zoom) || frame.view.zoom <= 0.0F ||
        !std::isfinite(frame.view.offset_x) ||
        !std::isfinite(frame.view.offset_y)) {
        return std::string("Live2D view values must be finite and zoom positive");
    }
    if (frame.tracking_valid && frame.source_face_frame_id == 0) {
        return std::string("A valid tracking frame requires a source face frame ID");
    }
    if (frame.source_timestamp_ns != 0 && frame.produced_timestamp_ns != 0 &&
        frame.produced_timestamp_ns < frame.source_timestamp_ns) {
        return std::string(
            "Parameter production timestamp precedes the source timestamp");
    }
    return std::nullopt;
}

std::optional<std::string> validate_render_frame(
    const GodotRenderFrame& frame) {
    if (frame.render_frame_id == 0) {
        return std::string("Completed render frame ID must be positive");
    }
    if (frame.width == 0 || frame.height == 0) {
        return std::string("Completed render dimensions must be positive");
    }
    if (frame.native_texture_handle == 0) {
        return std::string("Completed render texture handle is unavailable");
    }
    if (frame.iteration_finished_timestamp_ns <
        frame.iteration_started_timestamp_ns) {
        return std::string("Godot iteration timestamps are out of order");
    }
    if (frame.draw_completed_timestamp_ns <
        frame.iteration_finished_timestamp_ns) {
        return std::string("Draw completion timestamp precedes iteration completion");
    }
    return std::nullopt;
}

}  // namespace

const char* godot_runtime_state_name(GodotRuntimeState state) noexcept {
    switch (state) {
        case GodotRuntimeState::created:
            return "created";
        case GodotRuntimeState::initializing:
            return "initializing";
        case GodotRuntimeState::initialized:
            return "initialized";
        case GodotRuntimeState::model_loading:
            return "model_loading";
        case GodotRuntimeState::ready:
            return "ready";
        case GodotRuntimeState::running:
            return "running";
        case GodotRuntimeState::stopping:
            return "stopping";
        case GodotRuntimeState::stopped:
            return "stopped";
        case GodotRuntimeState::failed:
            return "failed";
    }
    return "unknown";
}

const char* godot_runtime_error_code_name(
    GodotRuntimeErrorCode code) noexcept {
    switch (code) {
        case GodotRuntimeErrorCode::none:
            return "none";
        case GodotRuntimeErrorCode::invalid_configuration:
            return "invalid_configuration";
        case GodotRuntimeErrorCode::invalid_frame_descriptor:
            return "invalid_frame_descriptor";
        case GodotRuntimeErrorCode::invalid_state:
            return "invalid_state";
        case GodotRuntimeErrorCode::godot_initialization_failed:
            return "godot_initialization_failed";
        case GodotRuntimeErrorCode::display_server_initialization_failed:
            return "display_server_initialization_failed";
        case GodotRuntimeErrorCode::gdextension_load_failed:
            return "gdextension_load_failed";
        case GodotRuntimeErrorCode::model_resource_load_failed:
            return "model_resource_load_failed";
        case GodotRuntimeErrorCode::scene_tree_state_error:
            return "scene_tree_state_error";
        case GodotRuntimeErrorCode::render_texture_unavailable:
            return "render_texture_unavailable";
        case GodotRuntimeErrorCode::rendering_failed:
            return "rendering_failed";
        case GodotRuntimeErrorCode::gpu_copy_failed:
            return "gpu_copy_failed";
        case GodotRuntimeErrorCode::rga_failed:
            return "rga_failed";
        case GodotRuntimeErrorCode::mpp_failed:
            return "mpp_failed";
        case GodotRuntimeErrorCode::output_failed:
            return "output_failed";
        case GodotRuntimeErrorCode::shutdown_timeout:
            return "shutdown_timeout";
        case GodotRuntimeErrorCode::backend_failure:
            return "backend_failure";
    }
    return "unknown";
}

GodotRuntimeError::GodotRuntimeError(
    GodotRuntimeErrorCode code,
    GodotRuntimeState state,
    std::string message,
    std::optional<std::int64_t> native_error_code)
    : code_(code),
      state_(state),
      message_(std::move(message)),
      native_error_code_(native_error_code) {}

GodotRuntimeErrorCode GodotRuntimeError::code() const noexcept {
    return code_;
}

GodotRuntimeState GodotRuntimeError::state() const noexcept {
    return state_;
}

const std::string& GodotRuntimeError::message() const noexcept {
    return message_;
}

const std::optional<std::int64_t>& GodotRuntimeError::native_error_code()
    const noexcept {
    return native_error_code_;
}

GodotRuntimeResult GodotRuntimeResult::success(GodotRuntimeState state) {
    return GodotRuntimeResult(state);
}

GodotRuntimeResult GodotRuntimeResult::failure(GodotRuntimeError error) {
    return GodotRuntimeResult(std::move(error));
}

GodotRuntimeResult::GodotRuntimeResult(GodotRuntimeState state) : state_(state) {}

GodotRuntimeResult::GodotRuntimeResult(GodotRuntimeError error)
    : ok_(false), state_(error.state()), error_(std::move(error)) {}

bool GodotRuntimeResult::ok() const noexcept { return ok_; }

GodotRuntimeState GodotRuntimeResult::state() const noexcept { return state_; }

const std::optional<GodotRuntimeError>& GodotRuntimeResult::error()
    const noexcept {
    return error_;
}

GodotRuntimeBackendResult GodotRuntimeBackendResult::success() {
    return GodotRuntimeBackendResult();
}

GodotRuntimeBackendResult GodotRuntimeBackendResult::failure(
    GodotRuntimeErrorCode code,
    std::string message,
    std::optional<std::int64_t> native_error_code) {
    return GodotRuntimeBackendResult(
        code, std::move(message), native_error_code);
}

GodotRuntimeBackendResult::GodotRuntimeBackendResult(
    GodotRuntimeErrorCode code,
    std::string message,
    std::optional<std::int64_t> native_error_code)
    : ok_(false),
      code_(code),
      message_(std::move(message)),
      native_error_code_(native_error_code) {}

bool GodotRuntimeBackendResult::ok() const noexcept { return ok_; }

GodotRuntimeErrorCode GodotRuntimeBackendResult::code() const noexcept {
    return code_;
}

const std::string& GodotRuntimeBackendResult::message() const noexcept {
    return message_;
}

const std::optional<std::int64_t>&
GodotRuntimeBackendResult::native_error_code() const noexcept {
    return native_error_code_;
}

GodotRuntime::GodotRuntime(std::unique_ptr<GodotRuntimeBackend> backend)
    : backend_(std::move(backend)) {
    if (!backend_) {
        throw std::invalid_argument("Godot runtime backend must not be null");
    }
}

GodotRuntime::~GodotRuntime() { shutdown_noexcept(); }

GodotRuntimeResult GodotRuntime::initialize(GodotRuntimeConfig config) {
    if (state() != GodotRuntimeState::created) {
        return invalid_state("initialize", GodotRuntimeState::created);
    }

    state_.store(GodotRuntimeState::initializing);
    const std::optional<std::string> validation_error = validate_config(config);
    if (validation_error) {
        return invalid_configuration(*validation_error);
    }
    config_ = std::move(config);

    try {
        GodotRuntimeBackendResult result = backend_->initialize(config_);
        if (!result.ok()) {
            return backend_failure(
                std::move(result), GodotRuntimeState::failed);
        }
    } catch (const std::exception& error) {
        return exception_failure(
            "initialize", error.what(), GodotRuntimeState::failed);
    } catch (...) {
        return exception_failure(
            "initialize", "unknown exception", GodotRuntimeState::failed);
    }

    state_.store(GodotRuntimeState::initialized);
    return GodotRuntimeResult::success(GodotRuntimeState::initialized);
}

GodotRuntimeResult GodotRuntime::load_model() {
    if (state() != GodotRuntimeState::initialized) {
        return invalid_state("load_model", GodotRuntimeState::initialized);
    }
    state_.store(GodotRuntimeState::model_loading);

    try {
        GodotRuntimeBackendResult result =
            backend_->load_model(config_.model_path);
        if (!result.ok()) {
            return backend_failure(
                std::move(result), GodotRuntimeState::failed);
        }
    } catch (const std::exception& error) {
        return exception_failure(
            "load_model", error.what(), GodotRuntimeState::failed);
    } catch (...) {
        return exception_failure(
            "load_model", "unknown exception", GodotRuntimeState::failed);
    }

    state_.store(GodotRuntimeState::ready);
    return GodotRuntimeResult::success(GodotRuntimeState::ready);
}

GodotRuntimeResult GodotRuntime::start() {
    if (state() != GodotRuntimeState::ready) {
        return invalid_state("start", GodotRuntimeState::ready);
    }

    try {
        GodotRuntimeBackendResult result = backend_->start();
        if (!result.ok()) {
            return backend_failure(
                std::move(result), GodotRuntimeState::failed);
        }
    } catch (const std::exception& error) {
        return exception_failure(
            "start", error.what(), GodotRuntimeState::failed);
    } catch (...) {
        return exception_failure(
            "start", "unknown exception", GodotRuntimeState::failed);
    }

    stop_requested_.store(false);
    state_.store(GodotRuntimeState::running);
    return GodotRuntimeResult::success(GodotRuntimeState::running);
}

GodotRuntimeResult GodotRuntime::submit_parameters(
    const Live2DParameterFrame& frame) {
    if (state() != GodotRuntimeState::running) {
        return invalid_state("submit_parameters", GodotRuntimeState::running);
    }
    if (stop_requested()) {
        GodotRuntimeError error(
            GodotRuntimeErrorCode::invalid_state,
            GodotRuntimeState::running,
            "submit_parameters is unavailable after stop was requested");
        remember_error(error);
        return GodotRuntimeResult::failure(std::move(error));
    }
    const std::optional<std::string> validation_error =
        validate_parameter_frame(frame);
    if (validation_error) {
        return invalid_frame(*validation_error);
    }

    try {
        GodotRuntimeBackendResult result =
            backend_->submit_parameters(frame);
        if (!result.ok()) {
            return backend_failure(
                std::move(result), GodotRuntimeState::failed);
        }
    } catch (const std::exception& error) {
        return exception_failure(
            "submit_parameters", error.what(), GodotRuntimeState::failed);
    } catch (...) {
        return exception_failure(
            "submit_parameters", "unknown exception", GodotRuntimeState::failed);
    }
    return GodotRuntimeResult::success(GodotRuntimeState::running);
}

GodotRuntimeResult GodotRuntime::step(
    std::uint64_t monotonic_timestamp_ns,
    std::optional<GodotRenderFrame>& completed_frame) {
    completed_frame.reset();
    if (state() != GodotRuntimeState::running) {
        return invalid_state("step", GodotRuntimeState::running);
    }
    if (stop_requested()) {
        state_.store(GodotRuntimeState::stopping);
        return GodotRuntimeResult::success(GodotRuntimeState::stopping);
    }

    try {
        GodotRuntimeBackendResult result =
            backend_->step(monotonic_timestamp_ns, completed_frame);
        if (!result.ok()) {
            completed_frame.reset();
            return backend_failure(
                std::move(result), GodotRuntimeState::failed);
        }
    } catch (const std::exception& error) {
        completed_frame.reset();
        return exception_failure(
            "step", error.what(), GodotRuntimeState::failed);
    } catch (...) {
        completed_frame.reset();
        return exception_failure(
            "step", "unknown exception", GodotRuntimeState::failed);
    }

    if (completed_frame) {
        const std::optional<std::string> validation_error =
            validate_render_frame(*completed_frame);
        if (validation_error) {
            completed_frame.reset();
            return backend_failure(
                GodotRuntimeBackendResult::failure(
                    GodotRuntimeErrorCode::render_texture_unavailable,
                    *validation_error),
                GodotRuntimeState::failed);
        }
    }
    return GodotRuntimeResult::success(GodotRuntimeState::running);
}

void GodotRuntime::request_stop() noexcept {
    stop_requested_.store(true);
}

GodotRuntimeResult GodotRuntime::shutdown() {
    if (state() == GodotRuntimeState::stopped) {
        return GodotRuntimeResult::success(GodotRuntimeState::stopped);
    }

    stop_requested_.store(true);
    state_.store(GodotRuntimeState::stopping);
    backend_->request_stop();

    try {
        GodotRuntimeBackendResult result = backend_->shutdown();
        if (!result.ok()) {
            return backend_failure(
                std::move(result), GodotRuntimeState::stopped);
        }
    } catch (const std::exception& error) {
        return exception_failure(
            "shutdown", error.what(), GodotRuntimeState::stopped);
    } catch (...) {
        return exception_failure(
            "shutdown", "unknown exception", GodotRuntimeState::stopped);
    }

    state_.store(GodotRuntimeState::stopped);
    return GodotRuntimeResult::success(GodotRuntimeState::stopped);
}

GodotRuntimeState GodotRuntime::state() const noexcept {
    return state_.load();
}

bool GodotRuntime::stop_requested() const noexcept {
    return stop_requested_.load();
}

std::optional<GodotRuntimeError> GodotRuntime::last_error() const {
    return last_error_;
}

GodotRuntimeResult GodotRuntime::invalid_state(
    const char* operation,
    GodotRuntimeState required_state) {
    const GodotRuntimeState current_state = state();
    std::string message(operation);
    message += " requires state ";
    message += godot_runtime_state_name(required_state);
    message += ", current state is ";
    message += godot_runtime_state_name(current_state);
    GodotRuntimeError error(
        GodotRuntimeErrorCode::invalid_state,
        current_state,
        std::move(message));
    remember_error(error);
    return GodotRuntimeResult::failure(std::move(error));
}

GodotRuntimeResult GodotRuntime::invalid_configuration(std::string message) {
    state_.store(GodotRuntimeState::failed);
    GodotRuntimeError error(
        GodotRuntimeErrorCode::invalid_configuration,
        GodotRuntimeState::failed,
        std::move(message));
    remember_error(error);
    return GodotRuntimeResult::failure(std::move(error));
}

GodotRuntimeResult GodotRuntime::invalid_frame(std::string message) {
    GodotRuntimeError error(
        GodotRuntimeErrorCode::invalid_frame_descriptor,
        state(),
        std::move(message));
    remember_error(error);
    return GodotRuntimeResult::failure(std::move(error));
}

GodotRuntimeResult GodotRuntime::backend_failure(
    GodotRuntimeBackendResult result,
    GodotRuntimeState failure_state) {
    state_.store(failure_state);
    GodotRuntimeErrorCode code = result.code();
    if (code == GodotRuntimeErrorCode::none) {
        code = GodotRuntimeErrorCode::backend_failure;
    }
    GodotRuntimeError error(
        code,
        failure_state,
        result.message(),
        result.native_error_code());
    remember_error(error);
    return GodotRuntimeResult::failure(std::move(error));
}

GodotRuntimeResult GodotRuntime::exception_failure(
    const char* operation,
    const char* detail,
    GodotRuntimeState failure_state) {
    state_.store(failure_state);
    std::string message(operation);
    message += " backend threw an exception: ";
    message += detail;
    GodotRuntimeError error(
        GodotRuntimeErrorCode::backend_failure,
        failure_state,
        std::move(message));
    remember_error(error);
    return GodotRuntimeResult::failure(std::move(error));
}

void GodotRuntime::remember_error(const GodotRuntimeError& error) {
    last_error_ = error;
}

void GodotRuntime::shutdown_noexcept() noexcept {
    try {
        shutdown();
    } catch (...) {
        state_.store(GodotRuntimeState::stopped);
    }
}

}  // namespace vtb
