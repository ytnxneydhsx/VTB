#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "live2d_tracking_snapshot.hpp"
#include "vtb/config/vtb_application_config.hpp"
#include "vtb/render/rgba_frame_sink.hpp"

namespace vtb {

/**
 * @brief 在调用线程内初始化、驱动并清理 LibGodot 的同步渲染任务主体。
 *
 * create() 只保存配置和值依赖，不调用 LibGodot。GodotRenderStage 把本对象的独占
 * 所有权移入工作线程后，再依次调用 initialize()、submit()、run_frame() 和
 * finish()。Pipeline 在 initialize() 中创建 LibGodotInstance，并在 finish() 中销毁；
 * 因此 LibGodotInstance 从创建到销毁始终位于同一个所有者线程。
 *
 * 本类不包含 LibGodot ABI、启动参数和资源清理实现，也不创建线程或持有消息通道。
 * 它从 LibGodotInstance 取得 RgbaFrameLease，并将其移动交给 GStreamer。
 * 最后一个编码输入引用释放后归还 GPU 槽位。槽位全部在用时跳过这次渲染。
 */
class GodotRenderPipeline final {
public:
    enum class FrameResult { skipped, rendered, finished };

    static std::unique_ptr<GodotRenderPipeline> create(
        const std::vector<std::string>& passthrough_arguments,
        unsigned long iteration_limit,
        RenderConfiguration config,
        RgbaFrameSink rgba_sink);

    ~GodotRenderPipeline();

    GodotRenderPipeline(const GodotRenderPipeline&) = delete;
    GodotRenderPipeline& operator=(const GodotRenderPipeline&) = delete;
    GodotRenderPipeline(GodotRenderPipeline&&) = delete;
    GodotRenderPipeline& operator=(GodotRenderPipeline&&) = delete;

    void initialize();
    void submit(const Live2DTrackingSnapshot& snapshot);
    FrameResult run_frame();
    void finish();

    unsigned long iteration_limit() const noexcept;
    std::chrono::nanoseconds frame_period() const noexcept;

private:
    class Impl;

    explicit GodotRenderPipeline(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

}  // namespace vtb
