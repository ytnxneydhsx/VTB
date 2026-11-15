#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "live2d_tracking_snapshot.hpp"
#include "vtb/config/vtb_application_config.hpp"
#include "vtb/hardware/rga/rga_executor.hpp"
#include "vtb/render/nv12_frame_sink.hpp"

namespace vtb {

class GodotRenderPipelineConfig final {
public:
    RenderConfiguration render;
    std::string metrics_path;
    std::chrono::milliseconds metrics_report_interval{60000};
};

/**
 * @brief 在调用线程内初始化、驱动并清理 LibGodot 的同步渲染任务主体。
 *
 * create() 只保存配置和值依赖，不调用 LibGodot。GodotRenderStage 把本对象的独占
 * 所有权移入工作线程后，再依次调用 initialize()、submit()、run_frame() 和
 * finish()。Pipeline 在 initialize() 中创建 LibGodotInstance，并在 finish() 中销毁；
 * 因此 LibGodotInstance 从创建到销毁始终位于同一个所有者线程。
 *
 * 本类不包含 LibGodot ABI、启动参数和资源清理实现，也不创建线程或持有消息通道。
 * 它编排独立的 LibGodotInstance 和 RgaNv12FrameTransformer，从实例取得同步借用的
 * RgbaFrameView，并在 Nv12FrameLease 有效期间同步调用 MediaEncodeSession 提供的
 * Nv12FrameSink。底层 RgbaFrameSink 回调只由 LibGodotInstance 内部接收；RGA Worker
 * 由共享 RgaExecutor 管理。
 */
class GodotRenderPipeline final {
public:
    static std::unique_ptr<GodotRenderPipeline> create(
        const std::vector<std::string>& passthrough_arguments,
        unsigned long iteration_limit,
        GodotRenderPipelineConfig config,
        std::shared_ptr<RgaExecutor> rga_executor,
        Nv12FrameSink nv12_sink);

    ~GodotRenderPipeline();

    GodotRenderPipeline(const GodotRenderPipeline&) = delete;
    GodotRenderPipeline& operator=(const GodotRenderPipeline&) = delete;
    GodotRenderPipeline(GodotRenderPipeline&&) = delete;
    GodotRenderPipeline& operator=(GodotRenderPipeline&&) = delete;

    void initialize();
    void submit(const Live2DTrackingSnapshot& snapshot);
    bool run_frame();
    std::uint64_t completed_frames() const noexcept;
    void finish();

    unsigned long iteration_limit() const noexcept;
    std::chrono::nanoseconds frame_period() const noexcept;

private:
    class Impl;

    explicit GodotRenderPipeline(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

}  // namespace vtb
