#include "vtb_application.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/extension/libgodot.h"
#include "face_diagnostic_session.hpp"
#include "live2d_control_server.hpp"
#include "live2d_tracking_session.hpp"
#include "media_output_session.hpp"
#include "openvt_libgodot_bridge.h"
#include "vtb/config/config_file_reader.hpp"
#include "vtb/config/vtb_application_config.hpp"
#include "vtb/face/openseeface_geometry_estimator.hpp"
#include "vtb/face/rga_image_transformer.hpp"
#include "vtb/face/rknn_face_models.hpp"
#include "vtb/output/rtsp_output.hpp"
#include "vtb/time/monotonic_clock.hpp"
#include "video_input_factory.hpp"

namespace vtb {
namespace {

std::string metrics_path(
    const InstrumentationConfiguration& config,
    const char* filename) {
    if (!config.metrics_persistence_enabled()) {
        return {};
    }
    return (std::filesystem::path(config.metrics_directory) / filename).string();
}

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
        if (openvt_libgodot_bridge_abi_version() != OPENVT_LIBGODOT_ABI_VERSION) {
            throw std::runtime_error("LibGodot bridge ABI mismatch; rebuild host and library together");
        }
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

    void start(
        const RenderConfiguration& config,
        EncodedVideoSink output_sink,
        const std::string& metrics_jsonl_path,
        std::chrono::milliseconds metrics_report_interval) {
        const int result = openvt_libgodot_start(instance_);
        if (result != 0) {
            throw std::runtime_error(
                "LibGodot start failed: " + std::to_string(result));
        }
        OpenVTEmbeddedApplicationConfig embedded_config{};
        embedded_config.live2d_model = config.live2d_model.c_str();
        embedded_config.abi_version = OPENVT_LIBGODOT_ABI_VERSION;
        embedded_config.config_size = sizeof(embedded_config);
        embedded_config.output_sink = output_sink;
        embedded_config.output_width = config.output_width;
        embedded_config.output_height = config.output_height;
        embedded_config.output_fps = config.output_fps;
        embedded_config.total_frames = config.total_frames;
        embedded_config.metrics_jsonl_path = metrics_jsonl_path.c_str();
        embedded_config.metrics_report_interval_ms =
            static_cast<std::uint32_t>(metrics_report_interval.count());
        // Also clean up a partially initialized application if initialization throws.
        application_cleanup_required_ = true;
        const int application_result =
            openvt_libgodot_application_initialize_v3(&embedded_config);
        if (application_result != 0) {
            throw_application_error(
                "C++ Live2D application initialization failed",
                application_result);
        }
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

    bool run_frame() {
        const int prepare_result =
            openvt_libgodot_application_prepare_frame(
                MonotonicClock::now_ns());
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

    int finish_application() noexcept {
        int application_result = 0;
        if (application_cleanup_required_) {
            application_result = openvt_libgodot_application_shutdown();
            application_cleanup_required_ = false;
        }
        return application_result;
    }

    int shutdown() noexcept {
        const int application_result = finish_application();
        if (instance_ != nullptr) {
            libgodot_destroy_godot_instance(instance_);
            instance_ = nullptr;
        }
        return application_result;
    }

private:
    [[noreturn]] static void throw_application_error(
        const std::string& message,
        int error_code) {
        throw std::runtime_error(
            message + ": " + std::to_string(error_code) + " " +
            openvt_libgodot_application_last_error());
    }

    GDExtensionObjectPtr instance_ = nullptr;
    bool application_cleanup_required_ = false;
};

/**
 * @brief 在一个专用所有者线程中管理完整的 LibGodot 生命周期和逐帧循环。
 *
 * LibGodot 的 Main::setup() 会释放初始主线程身份，GodotInstance::start() 调用的
 * Main::setup2() 会把当前调用线程登记为 Godot 主线程。因此本类在线程入口内部创建、
 * 启动、驱动并销毁 LibGodotInstance，其他线程只通过 Live2DTrackingSession 的现有
 * 线程安全快照传值，不直接访问 Godot 对象。
 *
 * 主控制线程在 start() 返回后启动跟踪和控制生产者，再用 begin_frames() 放行帧循环。
 * 帧循环结束后，主控制线程先停止这些生产者，最后调用 finish()，允许所有者线程按
 * Godot application、主输出会话、LibGodot instance 的顺序完成清理。
 */
class GodotRuntimeThread final {
public:
    GodotRuntimeThread(
        ApplicationArguments& arguments,
        RenderConfiguration render_config,
        EncodedVideoSink output_sink,
        std::string media_metrics_path,
        std::chrono::milliseconds metrics_report_interval,
        Live2DTrackingSession& tracking_session,
        MediaOutputSession& media_output,
        FaceDiagnosticSession* face_diagnostic,
        MediaOutputSession* face_diagnostic_output,
        const std::atomic<bool>& process_stop_requested)
        : arguments_(arguments),
          render_config_(std::move(render_config)),
          output_sink_(output_sink),
          media_metrics_path_(std::move(media_metrics_path)),
          metrics_report_interval_(metrics_report_interval),
          tracking_session_(tracking_session),
          media_output_(media_output),
          face_diagnostic_(face_diagnostic),
          face_diagnostic_output_(face_diagnostic_output),
          process_stop_requested_(process_stop_requested) {
        if ((face_diagnostic_ == nullptr) !=
            (face_diagnostic_output_ == nullptr)) {
            throw std::invalid_argument(
                "Face diagnostic session and output must be provided together");
        }
    }

    ~GodotRuntimeThread() {
        shutdown_noexcept();
    }

    GodotRuntimeThread(const GodotRuntimeThread&) = delete;
    GodotRuntimeThread& operator=(const GodotRuntimeThread&) = delete;
    GodotRuntimeThread(GodotRuntimeThread&&) = delete;
    GodotRuntimeThread& operator=(GodotRuntimeThread&&) = delete;

    /** @brief 创建所有者线程，并等待 LibGodot 在该线程内完成初始化。 */
    void start() {
        if (worker_.joinable()) {
            throw std::logic_error("Godot runtime thread is already started");
        }
        worker_ = std::thread(&GodotRuntimeThread::thread_main, this);

        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this]() { return initialization_finished_; });
        const std::exception_ptr initialization_error = failure_;
        lock.unlock();
        if (initialization_error) {
            worker_.join();
            std::rethrow_exception(initialization_error);
        }
    }

    /** @brief 在应用生产者全部启动后放行逐帧循环。 */
    void begin_frames() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            frames_permitted_ = true;
        }
        condition_.notify_all();
    }

    /** @brief 等待帧循环因信号、Godot结束、迭代上限或异步错误而退出。 */
    void wait_for_frame_loop() {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this]() { return frame_loop_finished_; });
    }

    /**
     * @brief 允许所有者线程清理 Godot 和输出资源，等待线程结束并传播失败。
     *
     * 调用方必须先停止控制、跟踪和可选诊断生产者，避免清理阶段产生新状态或输出。
     */
    void finish() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cleanup_permitted_ = true;
        }
        condition_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
        rethrow_if_failed();
    }

private:
    void thread_main() noexcept {
        try {
            LibGodotInstance godot(arguments_);
            godot.start(
                render_config_,
                output_sink_,
                media_metrics_path_,
                metrics_report_interval_);
            mark_initialization_finished();

            bool run_frames = false;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [this]() {
                    return frames_permitted_ || cleanup_permitted_;
                });
                run_frames = frames_permitted_ && !cleanup_permitted_;
            }

            if (run_frames) {
                try {
                    run_frame_loop(godot);
                } catch (...) {
                    remember_failure(std::current_exception());
                }
            }
            mark_frame_loop_finished();

            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [this]() { return cleanup_permitted_; });
            }
            finish_runtime(godot);
        } catch (...) {
            remember_failure(std::current_exception());
            mark_initialization_finished();
            mark_frame_loop_finished();
        }
    }

    void run_frame_loop(LibGodotInstance& godot) {
        auto next_frame_deadline = std::chrono::steady_clock::now();
        while (!process_stop_requested_.load(std::memory_order_acquire) &&
               !internal_stop_requested_.load(std::memory_order_acquire)) {
            std::optional<Live2DTrackingSnapshot> snapshot =
                tracking_session_.read_snapshot_if_changed();
            if (snapshot.has_value()) {
                godot.submit(snapshot.value());
            }

            bool finished = false;
            try {
                finished = godot.run_frame();
            } catch (...) {
                media_output_.rethrow_if_failed();
                throw;
            }
            media_output_.rethrow_if_failed();
            if (face_diagnostic_ != nullptr) {
                face_diagnostic_->rethrow_if_failed();
                face_diagnostic_output_->rethrow_if_failed();
            }

            ++iterations_;
            if (finished) {
                break;
            }
            if (arguments_.iteration_limit() > 0 &&
                iterations_ >= arguments_.iteration_limit()) {
                break;
            }

            next_frame_deadline += arguments_.frame_period();
            const auto current_time = std::chrono::steady_clock::now();
            if (next_frame_deadline < current_time) {
                next_frame_deadline = current_time;
            }
            std::this_thread::sleep_until(next_frame_deadline);
        }
        completed_frames_ = godot.completed_frames();
    }

    void finish_runtime(LibGodotInstance& godot) {
        if (completed_frames_ == 0) {
            completed_frames_ = godot.completed_frames();
        }
        std::cout << "VTB_LIBGODOT_RESULT iterations=" << iterations_
                  << " completed_frames=" << completed_frames_ << "\n";

        const int shutdown_result = godot.finish_application();
        try {
            media_output_.finish();
        } catch (...) {
            remember_failure(std::current_exception());
        }
        if (shutdown_result != 0) {
            try {
                throw std::runtime_error(
                "C++ Live2D application shutdown failed: " +
                std::to_string(shutdown_result) + " " +
                openvt_libgodot_application_last_error());
            } catch (...) {
                remember_failure(std::current_exception());
            }
        }
        godot.shutdown();
    }

    void mark_initialization_finished() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            initialization_finished_ = true;
        }
        condition_.notify_all();
    }

    void mark_frame_loop_finished() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            frame_loop_finished_ = true;
        }
        condition_.notify_all();
    }

    void remember_failure(std::exception_ptr failure) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!failure_) {
            failure_ = std::move(failure);
        }
    }

    void rethrow_if_failed() const {
        std::exception_ptr failure;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            failure = failure_;
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    void shutdown_noexcept() noexcept {
        internal_stop_requested_.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            frames_permitted_ = true;
            cleanup_permitted_ = true;
        }
        condition_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    ApplicationArguments& arguments_;
    RenderConfiguration render_config_;
    EncodedVideoSink output_sink_;
    std::string media_metrics_path_;
    std::chrono::milliseconds metrics_report_interval_;
    Live2DTrackingSession& tracking_session_;
    MediaOutputSession& media_output_;
    FaceDiagnosticSession* face_diagnostic_ = nullptr;
    MediaOutputSession* face_diagnostic_output_ = nullptr;
    const std::atomic<bool>& process_stop_requested_;
    std::atomic<bool> internal_stop_requested_{false};
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::exception_ptr failure_;
    std::thread worker_;
    std::uint64_t completed_frames_ = 0;
    unsigned long iterations_ = 0;
    bool initialization_finished_ = false;
    bool frames_permitted_ = false;
    bool frame_loop_finished_ = false;
    bool cleanup_permitted_ = false;
};

std::unique_ptr<FacePipeline> create_face_pipeline(
    const FacePipelineConfiguration& config) {
    auto image_transformer =
        std::make_unique<RgaImageTransformer>(config.rga_dma_heap);
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

std::unique_ptr<Live2DTrackingSession> create_tracking_session(
    const FaceMappingConfiguration& mapping,
    std::unique_ptr<VideoSource> video_input,
    std::unique_ptr<FacePipeline> face_pipeline,
    Live2DTrackingSession::ResultObserver result_observer) {
    Live2DMappingConfig mapping_config;
    mapping_config.calibration_frames =
        static_cast<std::size_t>(mapping.calibration_frames);
    mapping_config.smoothing_tau_ms = mapping.smoothing_tau_ms;
    mapping_config.angle_x_sign = mapping.angle_x_sign;
    mapping_config.angle_y_sign = mapping.angle_y_sign;
    mapping_config.angle_z_sign = mapping.angle_z_sign;

    return std::make_unique<Live2DTrackingSession>(
        std::move(video_input),
        std::move(face_pipeline),
        mapping_config,
        std::move(result_observer));
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
        // 第 1 阶段：装配摄像头输入、人脸流水线和可选 face3d 诊断链路。
        VideoInputFactory video_input_factory;
        std::unique_ptr<VideoSource> video_input =
            video_input_factory.create(config_.video_input);
        std::unique_ptr<FacePipeline> face_pipeline =
            create_face_pipeline(config_.face_pipeline);
        std::unique_ptr<MediaOutputSession> face_diagnostic_output;
        std::unique_ptr<FaceDiagnosticSession> face_diagnostic;
        Live2DTrackingSession::ResultObserver result_observer;
        const std::chrono::milliseconds metrics_report_interval(
            config_.instrumentation.report_interval_ms);
        if (config_.face_diagnostic_output.enabled) {
            const std::string diagnostic_metrics_path = metrics_path(
                config_.instrumentation,
                "vtb-face3d-output.metrics.jsonl");
            face_diagnostic_output = std::make_unique<MediaOutputSession>(
                std::make_unique<RtspOutput>(
                    config_.face_diagnostic_output.rtsp_url),
                diagnostic_metrics_path,
                metrics_report_interval,
                "face3d-output",
                "face3d-video",
                "vtb-face3d.output",
                "face3d.output_write");
            face_diagnostic = std::make_unique<FaceDiagnosticSession>(
                config_.face_diagnostic_output,
                config_.video_input.width,
                config_.video_input.height,
                config_.face_pipeline.rga_dma_heap,
                face_diagnostic_output->sink());
            FaceDiagnosticSession* const diagnostic = face_diagnostic.get();
            result_observer = [diagnostic](
                                  const std::optional<FaceTrackingResult>&
                                      result) {
                diagnostic->publish(result);
            };
        }
        std::unique_ptr<Live2DTrackingSession> tracking_session =
            create_tracking_session(
                config_.mapping,
                std::move(video_input),
                std::move(face_pipeline),
                std::move(result_observer));

        // 第 2 阶段：创建借用 tracking_session 的控制服务和两路输出上下文。
        Live2DControlServer control_server(config_.control, *tracking_session);
        const std::string output_metrics_path = metrics_path(
            config_.instrumentation,
            "vtb-live2d-output.metrics.jsonl");
        const std::string media_metrics_path = metrics_path(
            config_.instrumentation,
            "vtb-live2d-media.metrics.jsonl");
        // 局部对象按声明逆序析构，也能在异常展开时保护回调借用的上下文。
        MediaOutputSession media_output(
            std::make_unique<RtspOutput>(config_.output.rtsp_url),
            output_metrics_path,
            metrics_report_interval,
            "live2d-output",
            "live2d-video",
            "vtb-live2d.output",
            "live2d.output_write");
        GodotRuntimeThread godot_runtime(
            arguments_,
            config_.render,
            media_output.sink(),
            media_metrics_path,
            metrics_report_interval,
            *tracking_session,
            media_output,
            face_diagnostic.get(),
            face_diagnostic_output.get(),
            stop_requested);

        // 第 3 阶段：先在线程内部完成 LibGodot 初始化，再启动各生产者，最后放行帧循环。
        godot_runtime.start();
        if (face_diagnostic) {
            face_diagnostic->start();
        }
        tracking_session->start();
        control_server.start();
        godot_runtime.begin_frames();
        godot_runtime.wait_for_frame_loop();

        // 第 4 阶段：帧循环结束后先停止所有生产者，再允许 Godot 所有者线程清理并 join。
        control_server.stop();
        tracking_session->request_stop();
        tracking_session->stop();
        if (face_diagnostic) {
            face_diagnostic->request_stop();
            face_diagnostic->stop();
            face_diagnostic_output->finish();
        }
        godot_runtime.finish();
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
