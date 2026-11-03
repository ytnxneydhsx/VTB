#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "vtb/live2d/live2d_parameters.hpp"

namespace vtb {

enum class GodotRuntimeState {
    created,
    initializing,
    initialized,
    model_loading,
    ready,
    running,
    stopping,
    stopped,
    failed,
};

const char* godot_runtime_state_name(GodotRuntimeState state) noexcept;

enum class GodotRuntimeErrorCode {
    none,
    invalid_configuration,
    invalid_frame_descriptor,
    invalid_state,
    godot_initialization_failed,
    display_server_initialization_failed,
    gdextension_load_failed,
    model_resource_load_failed,
    scene_tree_state_error,
    render_texture_unavailable,
    rendering_failed,
    gpu_copy_failed,
    rga_failed,
    mpp_failed,
    output_failed,
    shutdown_timeout,
    backend_failure,
};

const char* godot_runtime_error_code_name(
    GodotRuntimeErrorCode code) noexcept;

enum class RenderPixelFormat {
    rgba8888,
};

class GodotRuntimeConfig {
public:
    GodotRuntimeConfig() = default;
    ~GodotRuntimeConfig() = default;

    GodotRuntimeConfig(const GodotRuntimeConfig&) = default;
    GodotRuntimeConfig& operator=(const GodotRuntimeConfig&) = default;
    GodotRuntimeConfig(GodotRuntimeConfig&&) noexcept = default;
    GodotRuntimeConfig& operator=(GodotRuntimeConfig&&) noexcept = default;

    std::string project_root;
    std::string model_path;
    std::string display_driver = "x11";
    std::string rendering_driver = "opengl3_es";
    std::string mali_library_path;
    std::uint32_t output_width = 1920;
    std::uint32_t output_height = 1080;
    std::uint32_t output_fps = 30;
    std::uint64_t maximum_parameter_age_ns = 150000000ULL;
    bool performance_profiling = false;
};

class Live2DViewState {
public:
    Live2DViewState() = default;
    ~Live2DViewState() = default;

    Live2DViewState(const Live2DViewState&) = default;
    Live2DViewState& operator=(const Live2DViewState&) = default;
    Live2DViewState(Live2DViewState&&) noexcept = default;
    Live2DViewState& operator=(Live2DViewState&&) noexcept = default;

    float zoom = 1.0F;
    float offset_x = 0.0F;
    float offset_y = 0.0F;
};

class Live2DParameterFrame {
public:
    Live2DParameterFrame() = default;
    ~Live2DParameterFrame() = default;

    Live2DParameterFrame(const Live2DParameterFrame&) = default;
    Live2DParameterFrame& operator=(const Live2DParameterFrame&) = default;
    Live2DParameterFrame(Live2DParameterFrame&&) noexcept = default;
    Live2DParameterFrame& operator=(Live2DParameterFrame&&) noexcept = default;

    std::uint64_t parameter_generation = 0;
    std::uint64_t source_face_frame_id = 0;
    std::uint64_t source_timestamp_ns = 0;
    std::uint64_t produced_timestamp_ns = 0;
    bool tracking_valid = false;
    bool calibrated = false;
    float confidence = 0.0F;
    Live2DParameters parameters;
    Live2DViewState view;
};

class GodotRenderFrame {
public:
    GodotRenderFrame() = default;
    ~GodotRenderFrame() = default;

    GodotRenderFrame(const GodotRenderFrame&) = default;
    GodotRenderFrame& operator=(const GodotRenderFrame&) = default;
    GodotRenderFrame(GodotRenderFrame&&) noexcept = default;
    GodotRenderFrame& operator=(GodotRenderFrame&&) noexcept = default;

    std::uint64_t render_frame_id = 0;
    std::uint64_t source_face_frame_id = 0;
    std::uint64_t parameter_generation = 0;
    std::uint64_t parameter_submitted_timestamp_ns = 0;
    std::uint64_t iteration_started_timestamp_ns = 0;
    std::uint64_t iteration_finished_timestamp_ns = 0;
    std::uint64_t draw_completed_timestamp_ns = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    RenderPixelFormat pixel_format = RenderPixelFormat::rgba8888;
    std::uintptr_t native_texture_handle = 0;
    bool reused_parameters = false;
    bool fallback_to_defaults = false;
};

class GodotRuntimeError {
public:
    GodotRuntimeError() = default;
    GodotRuntimeError(
        GodotRuntimeErrorCode code,
        GodotRuntimeState state,
        std::string message,
        std::optional<std::int64_t> native_error_code = std::nullopt);
    ~GodotRuntimeError() = default;

    GodotRuntimeError(const GodotRuntimeError&) = default;
    GodotRuntimeError& operator=(const GodotRuntimeError&) = default;
    GodotRuntimeError(GodotRuntimeError&&) noexcept = default;
    GodotRuntimeError& operator=(GodotRuntimeError&&) noexcept = default;

    GodotRuntimeErrorCode code() const noexcept;
    GodotRuntimeState state() const noexcept;
    const std::string& message() const noexcept;
    const std::optional<std::int64_t>& native_error_code() const noexcept;

private:
    GodotRuntimeErrorCode code_ = GodotRuntimeErrorCode::none;
    GodotRuntimeState state_ = GodotRuntimeState::created;
    std::string message_;
    std::optional<std::int64_t> native_error_code_;
};

class GodotRuntimeResult {
public:
    static GodotRuntimeResult success(GodotRuntimeState state);
    static GodotRuntimeResult failure(GodotRuntimeError error);

    ~GodotRuntimeResult() = default;

    GodotRuntimeResult(const GodotRuntimeResult&) = default;
    GodotRuntimeResult& operator=(const GodotRuntimeResult&) = default;
    GodotRuntimeResult(GodotRuntimeResult&&) noexcept = default;
    GodotRuntimeResult& operator=(GodotRuntimeResult&&) noexcept = default;

    bool ok() const noexcept;
    GodotRuntimeState state() const noexcept;
    const std::optional<GodotRuntimeError>& error() const noexcept;

private:
    explicit GodotRuntimeResult(GodotRuntimeState state);
    explicit GodotRuntimeResult(GodotRuntimeError error);

    bool ok_ = true;
    GodotRuntimeState state_ = GodotRuntimeState::created;
    std::optional<GodotRuntimeError> error_;
};

class GodotRuntimeBackendResult {
public:
    static GodotRuntimeBackendResult success();
    static GodotRuntimeBackendResult failure(
        GodotRuntimeErrorCode code,
        std::string message,
        std::optional<std::int64_t> native_error_code = std::nullopt);

    ~GodotRuntimeBackendResult() = default;

    GodotRuntimeBackendResult(const GodotRuntimeBackendResult&) = default;
    GodotRuntimeBackendResult& operator=(
        const GodotRuntimeBackendResult&) = default;
    GodotRuntimeBackendResult(GodotRuntimeBackendResult&&) noexcept = default;
    GodotRuntimeBackendResult& operator=(
        GodotRuntimeBackendResult&&) noexcept = default;

    bool ok() const noexcept;
    GodotRuntimeErrorCode code() const noexcept;
    const std::string& message() const noexcept;
    const std::optional<std::int64_t>& native_error_code() const noexcept;

private:
    GodotRuntimeBackendResult() = default;
    GodotRuntimeBackendResult(
        GodotRuntimeErrorCode code,
        std::string message,
        std::optional<std::int64_t> native_error_code);

    bool ok_ = true;
    GodotRuntimeErrorCode code_ = GodotRuntimeErrorCode::none;
    std::string message_;
    std::optional<std::int64_t> native_error_code_;
};

class GodotRuntimeBackend {
public:
    GodotRuntimeBackend() = default;
    virtual ~GodotRuntimeBackend() = default;

    GodotRuntimeBackend(const GodotRuntimeBackend&) = delete;
    GodotRuntimeBackend& operator=(const GodotRuntimeBackend&) = delete;
    GodotRuntimeBackend(GodotRuntimeBackend&&) = delete;
    GodotRuntimeBackend& operator=(GodotRuntimeBackend&&) = delete;

    virtual GodotRuntimeBackendResult initialize(
        const GodotRuntimeConfig& config) = 0;
    virtual GodotRuntimeBackendResult load_model(
        const std::string& model_path) = 0;
    virtual GodotRuntimeBackendResult start() = 0;
    virtual GodotRuntimeBackendResult submit_parameters(
        const Live2DParameterFrame& frame) = 0;
    virtual GodotRuntimeBackendResult step(
        std::uint64_t monotonic_timestamp_ns,
        std::optional<GodotRenderFrame>& completed_frame) = 0;
    virtual void request_stop() noexcept = 0;
    virtual GodotRuntimeBackendResult shutdown() = 0;
};

class GodotRuntime {
public:
    explicit GodotRuntime(std::unique_ptr<GodotRuntimeBackend> backend);
    ~GodotRuntime();

    GodotRuntime(const GodotRuntime&) = delete;
    GodotRuntime& operator=(const GodotRuntime&) = delete;
    GodotRuntime(GodotRuntime&&) = delete;
    GodotRuntime& operator=(GodotRuntime&&) = delete;

    GodotRuntimeResult initialize(GodotRuntimeConfig config);
    GodotRuntimeResult load_model();
    GodotRuntimeResult start();
    GodotRuntimeResult submit_parameters(const Live2DParameterFrame& frame);
    GodotRuntimeResult step(
        std::uint64_t monotonic_timestamp_ns,
        std::optional<GodotRenderFrame>& completed_frame);
    void request_stop() noexcept;
    GodotRuntimeResult shutdown();

    GodotRuntimeState state() const noexcept;
    bool stop_requested() const noexcept;
    std::optional<GodotRuntimeError> last_error() const;

private:
    GodotRuntimeResult invalid_state(
        const char* operation,
        GodotRuntimeState required_state);
    GodotRuntimeResult invalid_configuration(std::string message);
    GodotRuntimeResult invalid_frame(std::string message);
    GodotRuntimeResult backend_failure(
        GodotRuntimeBackendResult result,
        GodotRuntimeState failure_state);
    GodotRuntimeResult exception_failure(
        const char* operation,
        const char* detail,
        GodotRuntimeState failure_state);
    void remember_error(const GodotRuntimeError& error);
    void shutdown_noexcept() noexcept;

    std::unique_ptr<GodotRuntimeBackend> backend_;
    GodotRuntimeConfig config_;
    std::atomic<GodotRuntimeState> state_{GodotRuntimeState::created};
    std::atomic<bool> stop_requested_{false};
    std::optional<GodotRuntimeError> last_error_;
};

}  // namespace vtb
