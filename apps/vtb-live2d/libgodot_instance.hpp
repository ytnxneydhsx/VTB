#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "live2d_tracking_snapshot.hpp"
#include "vtb/config/vtb_application_config.hpp"
#include "vtb/render/rgba_frame_sink.hpp"

namespace vtb {

/**
 * @brief 独占一个嵌入式 LibGodot 实例及 OpenVT ABI 应用生命周期。
 *
 * 本类封装 Godot 进程参数、ABI 版本检查、实例创建、Live2D 参数提交、渲染原子步骤
 * 和有序销毁。调用方必须在同一个 Godot 所有者线程内构造、调用并析构本对象。
 */
class LibGodotInstance final {
public:
    LibGodotInstance(
        const std::vector<std::string>& passthrough_arguments,
        RenderConfiguration render_config);
    ~LibGodotInstance();

    LibGodotInstance(const LibGodotInstance&) = delete;
    LibGodotInstance& operator=(const LibGodotInstance&) = delete;
    LibGodotInstance(LibGodotInstance&&) = delete;
    LibGodotInstance& operator=(LibGodotInstance&&) = delete;

    void start();
    void submit(const Live2DTrackingSnapshot& snapshot);
    void prepare_frame(std::uint64_t timestamp_ns);
    bool iterate();
    void wait_gpu_render();
    /**
     * @brief 完成本帧发布并返回可同步借用的 RGBA DMA-BUF 描述。
     *
     * 返回内容在下一次 iterate() 或本实例销毁前保持不变。
     */
    RgbaFrameView publish_rgba_frame(std::uint64_t timestamp_ns);
    bool should_stop() const noexcept;
    void finish();
    void shutdown() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vtb
