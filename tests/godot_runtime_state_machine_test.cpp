#include "vtb/render/godot_runtime.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using vtb::GodotRenderFrame;
using vtb::GodotRuntime;
using vtb::GodotRuntimeBackend;
using vtb::GodotRuntimeBackendResult;
using vtb::GodotRuntimeConfig;
using vtb::GodotRuntimeErrorCode;
using vtb::GodotRuntimeState;
using vtb::Live2DParameterFrame;

void expect(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

enum class MockFailurePoint {
    none,
    initialize,
    load_model,
    start,
    submit_parameters,
    step,
    shutdown,
};

class MockGodotRuntimeBackend final : public GodotRuntimeBackend {
public:
    MockGodotRuntimeBackend() = default;
    ~MockGodotRuntimeBackend() override = default;

    MockGodotRuntimeBackend(const MockGodotRuntimeBackend&) = delete;
    MockGodotRuntimeBackend& operator=(const MockGodotRuntimeBackend&) = delete;
    MockGodotRuntimeBackend(MockGodotRuntimeBackend&&) = delete;
    MockGodotRuntimeBackend& operator=(MockGodotRuntimeBackend&&) = delete;

    GodotRuntimeBackendResult initialize(
        const GodotRuntimeConfig& config) override {
        ++initialize_calls;
        observed_config = config;
        if (failure_point == MockFailurePoint::initialize) {
            return GodotRuntimeBackendResult::failure(
                GodotRuntimeErrorCode::display_server_initialization_failed,
                "mock display initialization failed",
                17);
        }
        return GodotRuntimeBackendResult::success();
    }

    GodotRuntimeBackendResult load_model(
        const std::string& model_path) override {
        ++load_model_calls;
        observed_model_path = model_path;
        if (failure_point == MockFailurePoint::load_model) {
            return GodotRuntimeBackendResult::failure(
                GodotRuntimeErrorCode::model_resource_load_failed,
                "mock model load failed");
        }
        return GodotRuntimeBackendResult::success();
    }

    GodotRuntimeBackendResult start() override {
        ++start_calls;
        if (failure_point == MockFailurePoint::start) {
            return GodotRuntimeBackendResult::failure(
                GodotRuntimeErrorCode::scene_tree_state_error,
                "mock start failed");
        }
        return GodotRuntimeBackendResult::success();
    }

    GodotRuntimeBackendResult submit_parameters(
        const Live2DParameterFrame& frame) override {
        ++submit_calls;
        observed_parameter_frame = frame;
        if (failure_point == MockFailurePoint::submit_parameters) {
            return GodotRuntimeBackendResult::failure(
                GodotRuntimeErrorCode::scene_tree_state_error,
                "mock parameter submission failed");
        }
        return GodotRuntimeBackendResult::success();
    }

    GodotRuntimeBackendResult step(
        std::uint64_t monotonic_timestamp_ns,
        std::optional<GodotRenderFrame>& completed_frame) override {
        ++step_calls;
        observed_step_timestamp_ns = monotonic_timestamp_ns;
        if (failure_point == MockFailurePoint::step) {
            return GodotRuntimeBackendResult::failure(
                GodotRuntimeErrorCode::rendering_failed,
                "mock iteration failed");
        }
        if (!produce_completed_frame) {
            completed_frame.reset();
            return GodotRuntimeBackendResult::success();
        }

        GodotRenderFrame frame;
        frame.render_frame_id = 9;
        frame.source_face_frame_id = 7;
        frame.parameter_generation = 5;
        frame.parameter_submitted_timestamp_ns = monotonic_timestamp_ns - 30;
        frame.iteration_started_timestamp_ns = monotonic_timestamp_ns - 20;
        frame.iteration_finished_timestamp_ns = monotonic_timestamp_ns - 10;
        frame.draw_completed_timestamp_ns = monotonic_timestamp_ns;
        frame.width = observed_config.output_width;
        frame.height = observed_config.output_height;
        frame.native_texture_handle = 1234;
        completed_frame = frame;
        return GodotRuntimeBackendResult::success();
    }

    void request_stop() noexcept override { ++request_stop_calls; }

    GodotRuntimeBackendResult shutdown() override {
        ++shutdown_calls;
        if (failure_point == MockFailurePoint::shutdown) {
            return GodotRuntimeBackendResult::failure(
                GodotRuntimeErrorCode::shutdown_timeout,
                "mock shutdown timed out");
        }
        return GodotRuntimeBackendResult::success();
    }

    MockFailurePoint failure_point = MockFailurePoint::none;
    bool produce_completed_frame = true;
    int initialize_calls = 0;
    int load_model_calls = 0;
    int start_calls = 0;
    int submit_calls = 0;
    int step_calls = 0;
    int request_stop_calls = 0;
    int shutdown_calls = 0;
    std::uint64_t observed_step_timestamp_ns = 0;
    GodotRuntimeConfig observed_config;
    std::string observed_model_path;
    Live2DParameterFrame observed_parameter_frame;
};

GodotRuntimeConfig valid_config() {
    GodotRuntimeConfig config;
    config.project_root = "/opt/vtb/ayagami";
    config.model_path = "res://models/character/model3.json";
    config.mali_library_path = "/opt/rockchip/libmali";
    return config;
}

Live2DParameterFrame valid_parameter_frame() {
    Live2DParameterFrame frame;
    frame.parameter_generation = 5;
    frame.source_face_frame_id = 7;
    frame.source_timestamp_ns = 1000;
    frame.produced_timestamp_ns = 1100;
    frame.tracking_valid = true;
    frame.calibrated = true;
    frame.confidence = 0.9F;
    return frame;
}

void test_successful_lifecycle_and_frame_forwarding() {
    auto backend = std::make_unique<MockGodotRuntimeBackend>();
    MockGodotRuntimeBackend* observed_backend = backend.get();
    GodotRuntime runtime(std::move(backend));

    expect(runtime.state() == GodotRuntimeState::created, "initial state");
    const auto early_start = runtime.start();
    expect(!early_start.ok(), "start before initialization must fail");
    expect(
        early_start.error()->code() == GodotRuntimeErrorCode::invalid_state,
        "early start error category");
    expect(runtime.state() == GodotRuntimeState::created, "invalid call state");

    expect(runtime.initialize(valid_config()).ok(), "initialize succeeds");
    expect(
        runtime.state() == GodotRuntimeState::initialized,
        "initialized state");
    expect(observed_backend->initialize_calls == 1, "initialize called once");

    const auto duplicate_initialize = runtime.initialize(valid_config());
    expect(!duplicate_initialize.ok(), "initialize is one-shot");
    expect(
        runtime.state() == GodotRuntimeState::initialized,
        "duplicate initialize preserves state");

    expect(runtime.load_model().ok(), "model load succeeds");
    expect(runtime.state() == GodotRuntimeState::ready, "ready state");
    expect(
        observed_backend->observed_model_path ==
            "res://models/character/model3.json",
        "model path forwarded");

    expect(runtime.start().ok(), "runtime starts");
    expect(runtime.state() == GodotRuntimeState::running, "running state");

    const Live2DParameterFrame parameter_frame = valid_parameter_frame();
    expect(
        runtime.submit_parameters(parameter_frame).ok(),
        "parameter submission succeeds");
    expect(observed_backend->submit_calls == 1, "parameter forwarded once");
    expect(
        observed_backend->observed_parameter_frame.parameter_generation == 5,
        "parameter generation forwarded");

    std::optional<GodotRenderFrame> completed_frame;
    expect(runtime.step(2000, completed_frame).ok(), "step succeeds");
    expect(completed_frame.has_value(), "step returns completed frame");
    expect(completed_frame->render_frame_id == 9, "render frame ID retained");
    expect(
        completed_frame->native_texture_handle == 1234,
        "native texture handle retained");

    runtime.request_stop();
    expect(runtime.stop_requested(), "stop request recorded atomically");
    completed_frame = GodotRenderFrame();
    const auto stopping_step = runtime.step(2100, completed_frame);
    expect(stopping_step.ok(), "step observes stop request");
    expect(
        stopping_step.state() == GodotRuntimeState::stopping,
        "stop request advances state");
    expect(!completed_frame.has_value(), "stopping step yields no frame");
    expect(observed_backend->step_calls == 1, "backend step is not called again");

    expect(runtime.shutdown().ok(), "shutdown succeeds");
    expect(runtime.state() == GodotRuntimeState::stopped, "stopped state");
    expect(observed_backend->request_stop_calls == 1, "backend stop signalled");
    expect(observed_backend->shutdown_calls == 1, "backend shut down once");

    expect(runtime.shutdown().ok(), "shutdown is idempotent");
    expect(observed_backend->shutdown_calls == 1, "shutdown remains one-shot");
}

void test_invalid_configuration_does_not_touch_backend() {
    auto backend = std::make_unique<MockGodotRuntimeBackend>();
    MockGodotRuntimeBackend* observed_backend = backend.get();
    GodotRuntime runtime(std::move(backend));

    GodotRuntimeConfig config = valid_config();
    config.project_root.clear();
    const auto result = runtime.initialize(std::move(config));
    expect(!result.ok(), "invalid configuration fails");
    expect(
        result.error()->code() ==
            GodotRuntimeErrorCode::invalid_configuration,
        "invalid configuration category");
    expect(runtime.state() == GodotRuntimeState::failed, "failed state");
    expect(observed_backend->initialize_calls == 0, "backend untouched");
    expect(runtime.shutdown().ok(), "failed runtime can shut down");
}

void test_partial_initialization_failure_is_cleaned_up() {
    auto backend = std::make_unique<MockGodotRuntimeBackend>();
    backend->failure_point = MockFailurePoint::initialize;
    MockGodotRuntimeBackend* observed_backend = backend.get();
    GodotRuntime runtime(std::move(backend));

    const auto result = runtime.initialize(valid_config());
    expect(!result.ok(), "mock initialization failure reported");
    expect(
        result.error()->code() ==
            GodotRuntimeErrorCode::display_server_initialization_failed,
        "backend error category retained");
    expect(result.error()->native_error_code().has_value(), "native code retained");
    expect(runtime.state() == GodotRuntimeState::failed, "failed state retained");

    expect(runtime.shutdown().ok(), "partial initialization is cleaned up");
    expect(observed_backend->request_stop_calls == 1, "cleanup requests stop");
    expect(observed_backend->shutdown_calls == 1, "cleanup reaches backend");
}

void test_invalid_parameter_frame_does_not_fail_runtime() {
    auto backend = std::make_unique<MockGodotRuntimeBackend>();
    MockGodotRuntimeBackend* observed_backend = backend.get();
    GodotRuntime runtime(std::move(backend));
    expect(runtime.initialize(valid_config()).ok(), "initialize");
    expect(runtime.load_model().ok(), "load model");
    expect(runtime.start().ok(), "start");

    Live2DParameterFrame frame = valid_parameter_frame();
    frame.parameter_generation = 0;
    const auto result = runtime.submit_parameters(frame);
    expect(!result.ok(), "invalid parameter frame rejected");
    expect(
        result.error()->code() ==
            GodotRuntimeErrorCode::invalid_frame_descriptor,
        "frame error category");
    expect(runtime.state() == GodotRuntimeState::running, "runtime stays running");
    expect(observed_backend->submit_calls == 0, "invalid frame not forwarded");
    expect(runtime.shutdown().ok(), "shutdown after invalid input");
}

void test_invalid_completed_frame_fails_runtime() {
    auto backend = std::make_unique<MockGodotRuntimeBackend>();
    MockGodotRuntimeBackend* observed_backend = backend.get();
    GodotRuntime runtime(std::move(backend));
    expect(runtime.initialize(valid_config()).ok(), "initialize");
    expect(runtime.load_model().ok(), "load model");
    expect(runtime.start().ok(), "start");

    observed_backend->observed_config.output_width = 0;
    std::optional<GodotRenderFrame> completed_frame;
    const auto result = runtime.step(2000, completed_frame);
    expect(!result.ok(), "invalid completed frame rejected");
    expect(
        result.error()->code() ==
            GodotRuntimeErrorCode::render_texture_unavailable,
        "completed frame error category");
    expect(runtime.state() == GodotRuntimeState::failed, "runtime fails closed");
    expect(!completed_frame.has_value(), "invalid completed frame discarded");
    expect(runtime.shutdown().ok(), "failed render runtime shuts down");
}

void test_shutdown_failure_still_reaches_stopped_state() {
    auto backend = std::make_unique<MockGodotRuntimeBackend>();
    MockGodotRuntimeBackend* observed_backend = backend.get();
    GodotRuntime runtime(std::move(backend));
    expect(runtime.initialize(valid_config()).ok(), "initialize");
    observed_backend->failure_point = MockFailurePoint::shutdown;

    const auto result = runtime.shutdown();
    expect(!result.ok(), "shutdown failure reported");
    expect(
        result.error()->code() == GodotRuntimeErrorCode::shutdown_timeout,
        "shutdown error category");
    expect(runtime.state() == GodotRuntimeState::stopped, "stopped after failure");
    expect(runtime.shutdown().ok(), "repeated shutdown is harmless");
    expect(observed_backend->shutdown_calls == 1, "failed shutdown not repeated");
}

}  // namespace

int main() {
    test_successful_lifecycle_and_frame_forwarding();
    test_invalid_configuration_does_not_touch_backend();
    test_partial_initialization_failure_is_cleaned_up();
    test_invalid_parameter_frame_does_not_fail_runtime();
    test_invalid_completed_frame_fails_runtime();
    test_shutdown_failure_still_reaches_stopped_state();
    return 0;
}
