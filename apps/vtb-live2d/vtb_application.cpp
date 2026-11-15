#include "vtb_application.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "live2d_control_server.hpp"
#include "media_encode_session.hpp"
#include "media_output_session.hpp"
#include "pipeline/face_tracking_stage.hpp"
#include "pipeline/godot_render_pipeline.hpp"
#include "pipeline/godot_render_stage.hpp"
#include "pipeline/live2d_tracking_pipeline.hpp"
#include "pipeline/video_capture_stage.hpp"
#include "vtb/config/config_file_reader.hpp"
#include "vtb/config/vtb_application_config.hpp"
#include "vtb/hardware/rga/rga_executor.hpp"
#include "vtb/output/rtsp_output.hpp"
#include "vtb/output/gstreamer_uvc_output.hpp"
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
                    throw std::invalid_argument(
                        "--config may only be specified once");
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

void remember_first_failure(
    std::exception_ptr& first_failure,
    std::exception_ptr failure) noexcept {
    if (!first_failure) {
        first_failure = std::move(failure);
    }
}

}  // namespace

class VtbApplication::Impl final {
public:
    Impl(int argc, char** argv)
        : command_line_(argc, argv),
          config_(load_application_configuration(command_line_.config_path())) {}

    ~Impl() = default;

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    int run(const std::atomic<bool>& stop_requested) {
        // 第 1 阶段：创建输入、通道和所有宿主侧下游资源。
        VideoInputFactory video_input_factory;
        std::unique_ptr<VideoSource> video_input =
            video_input_factory.create(config_.video_input);

        const std::chrono::milliseconds metrics_report_interval(
            config_.instrumentation.report_interval_ms);
        VideoFrameChannel frame_channel(
            1, messaging::ChannelOverflowPolicy::drop_oldest);
        Live2DTrackingChannel tracking_channel(
            1, messaging::ChannelOverflowPolicy::drop_oldest);

        const std::string output_metrics_path = metrics_path(
            config_.instrumentation,
            "vtb-live2d-output.metrics.jsonl");
        const std::string render_metrics_path = metrics_path(
            config_.instrumentation,
            "vtb-live2d-render.metrics.jsonl");
        const std::string encode_metrics_path = metrics_path(
            config_.instrumentation,
            "vtb-live2d-encode.metrics.jsonl");
        const std::string rga_metrics_path = metrics_path(
            config_.instrumentation,
            "vtb-live2d-rga.metrics.jsonl");
        std::unique_ptr<Output> uvc_output;
        if (config_.output.uvc_enabled) {
            GstreamerUvcOutputConfig uvc_config;
            uvc_config.gst_launch_path = config_.output.gst_launch_path;
            uvc_config.device_path = config_.output.uvc_device;
            uvc_config.library_path = config_.output.gstreamer_library_path;
            uvc_config.plugin_path = config_.output.gstreamer_plugin_path;
            uvc_output = std::make_unique<GstreamerUvcOutput>(
                std::move(uvc_config));
        }
        MediaOutputSessionConfig media_output_config;
        media_output_config.metrics_path = output_metrics_path;
        media_output_config.metrics_report_interval = metrics_report_interval;
        MediaOutputSession media_output(
            std::make_unique<RtspOutput>(config_.output.rtsp_url),
            std::move(uvc_output),
            std::move(media_output_config));
        MediaEncodeSessionConfig media_encode_config;
        media_encode_config.uvc_fps = config_.output.uvc_fps;
        media_encode_config.metrics_path = encode_metrics_path;
        media_encode_config.metrics_report_interval = metrics_report_interval;
        MediaEncodeSession media_encode(
            media_output.sink(), std::move(media_encode_config));
        RgaExecutorConfig rga_executor_config;
        rga_executor_config.dma_heap_path =
            config_.face_pipeline.rga_dma_heap;
        rga_executor_config.worker_count = 2;
        rga_executor_config.queue_capacity = 4;
        rga_executor_config.metrics_path = rga_metrics_path;
        rga_executor_config.metrics_report_interval = metrics_report_interval;
        std::shared_ptr<RgaExecutor> rga_executor =
            std::make_shared<RgaExecutor>(std::move(rga_executor_config));

        std::unique_ptr<FacePipeline> face_pipeline =
            FacePipeline::create(config_.face_pipeline, rga_executor);
        std::unique_ptr<Live2DTrackingPipeline> tracking_pipeline =
            std::make_unique<Live2DTrackingPipeline>(
                std::move(face_pipeline), config_.mapping);

        // 第 2 阶段：Stage 取得处理主体并绑定输入、输出通道。
        FaceTrackingStage face_tracking_stage(
            std::move(tracking_pipeline),
            frame_channel,
            tracking_channel);
        VideoCaptureStage video_capture_stage(
            std::move(video_input), frame_channel);
        Live2DControlServer control_server(
            config_.control, face_tracking_stage);

        // create() 装配渲染、共享 RGA Executor 与 NV12 同步接收器。Stage 的所有者
        // 线程负责 LibGodot；RGA 命令由 Executor Worker 执行，结果返回后同步进入 MPP。
        GodotRenderPipelineConfig godot_render_config;
        godot_render_config.render = config_.render;
        godot_render_config.metrics_path = render_metrics_path;
        godot_render_config.metrics_report_interval = metrics_report_interval;
        std::unique_ptr<GodotRenderPipeline> godot_render_pipeline =
            GodotRenderPipeline::create(
                command_line_.passthrough_arguments(),
                command_line_.iteration_limit(),
                std::move(godot_render_config),
                rga_executor,
                media_encode.sink());
        GodotRenderStage godot_render_stage(
            std::move(godot_render_pipeline),
            tracking_channel,
            stop_requested);

        std::exception_ptr first_failure;
        try {
            // 第 3 阶段：先初始化下游，再启动生产者，最后放行渲染帧循环。
            godot_render_stage.start();
            face_tracking_stage.start();
            video_capture_stage.start();
            control_server.start();
            godot_render_stage.begin_frames();
            godot_render_stage.wait_for_frame_loop();
        } catch (...) {
            remember_first_failure(first_failure, std::current_exception());
        }

        // 第 4 阶段：停止外部入口与生产者后，再清理并回收 Godot 所有者线程。
        control_server.stop();
        video_capture_stage.request_stop();
        try {
            video_capture_stage.stop();
        } catch (...) {
            remember_first_failure(first_failure, std::current_exception());
        }
        try {
            face_tracking_stage.stop();
        } catch (...) {
            remember_first_failure(first_failure, std::current_exception());
        }
        try {
            video_capture_stage.rethrow_if_failed();
        } catch (...) {
            remember_first_failure(first_failure, std::current_exception());
        }
        try {
            face_tracking_stage.rethrow_if_failed();
        } catch (...) {
            remember_first_failure(first_failure, std::current_exception());
        }
        try {
            godot_render_stage.finish();
        } catch (...) {
            remember_first_failure(first_failure, std::current_exception());
        }
        try {
            rga_executor->finish();
        } catch (...) {
            remember_first_failure(first_failure, std::current_exception());
        }
        try {
            media_encode.finish();
        } catch (...) {
            remember_first_failure(first_failure, std::current_exception());
        }
        try {
            media_output.finish();
        } catch (...) {
            remember_first_failure(first_failure, std::current_exception());
        }
        if (first_failure) {
            std::rethrow_exception(first_failure);
        }
        return 0;
    }

private:
    CommandLineArguments command_line_;
    VtbApplicationConfiguration config_;
};

VtbApplication::VtbApplication(int argc, char** argv)
    : impl_(std::make_unique<Impl>(argc, argv)) {}

VtbApplication::~VtbApplication() = default;

int VtbApplication::run(const std::atomic<bool>& stop_requested) {
    return impl_->run(stop_requested);
}

}  // namespace vtb
