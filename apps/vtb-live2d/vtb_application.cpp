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

#include "gstreamer_media_session.hpp"
#include "live2d_control_server.hpp"
#include "pipeline/face_tracking_stage.hpp"
#include "pipeline/godot_render_pipeline.hpp"
#include "pipeline/godot_render_stage.hpp"
#include "pipeline/live2d_tracking_pipeline.hpp"
#include "pipeline/video_capture_stage.hpp"
#include "vtb/config/config_file_reader.hpp"
#include "vtb/config/vtb_application_config.hpp"
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

        const std::string render_metrics_path = metrics_path(
            config_.instrumentation,
            "vtb-live2d-render.metrics.jsonl");
        const std::string media_metrics_path = metrics_path(
            config_.instrumentation,
            "vtb-live2d-gstreamer-media.metrics.jsonl");
        GStreamerMediaSessionConfig media_config;
        media_config.rtsp_enabled = config_.output.rtsp_enabled;
        media_config.rtsp_url = config_.output.rtsp_url;
        media_config.webrtc_enabled = config_.output.webrtc_enabled;
        media_config.webrtc_web_directory = config_.output.webrtc_web_directory;
        media_config.webrtc_bind_address = config_.output.webrtc_bind_address;
        media_config.uvc_enabled = config_.output.uvc_enabled;
        media_config.uvc_device = config_.output.uvc_device;
        media_config.uvc_link_path = config_.output.uvc_link_path;
        media_config.uvc_fps = config_.output.uvc_fps;
        media_config.plugin_path = config_.output.gstreamer_plugin_path;
        media_config.metrics_path = media_metrics_path;
        media_config.metrics_report_interval = metrics_report_interval;
        GStreamerMediaSession media(std::move(media_config));
        std::unique_ptr<FacePipeline> face_pipeline =
            FacePipeline::create(config_.face_pipeline);
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

        // create() 装配渲染、线程内同步 RGA 转换与异步 GStreamer 接收器。
        // Godot 所有者线程移动交付 Nv12FrameLease；编码和输出由 GStreamer 分支完成。
        GodotRenderPipelineConfig godot_render_config;
        godot_render_config.render = config_.render;
        godot_render_config.rga_dma_heap_path =
            config_.face_pipeline.rga_dma_heap;
        godot_render_config.metrics_path = render_metrics_path;
        godot_render_config.metrics_report_interval = metrics_report_interval;
        std::unique_ptr<GodotRenderPipeline> godot_render_pipeline =
            GodotRenderPipeline::create(
                command_line_.passthrough_arguments(),
                command_line_.iteration_limit(),
                std::move(godot_render_config),
                media.sink());
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
            media.finish();
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
