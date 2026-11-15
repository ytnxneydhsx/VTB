#pragma once

#include <memory>
#include <string>

#include "vtb/hardware/rga/rga_transform_session.hpp"
#include "vtb/render/nv12_frame_sink.hpp"
#include "vtb/render/rgba_frame_sink.hpp"

namespace vtb {

/**
 * @brief 把 LibGodot 发布的 RGBA DMA-BUF 同步转换为带槽位租约的 NV12 帧。
 *
 * 本类直接持有独立 RgaTransformSession，不创建线程，也不定义第二套 NV12
 * 转换抽象。transform() 在 Godot 所有者线程直接同步调用 librga；四槽目标池
 * 允许 GStreamer 异步持有少量在途帧。
 */
class RgaNv12FrameTransformer final {
public:
    explicit RgaNv12FrameTransformer(std::string dma_heap_path);
    ~RgaNv12FrameTransformer();

    RgaNv12FrameTransformer(const RgaNv12FrameTransformer&) = delete;
    RgaNv12FrameTransformer& operator=(const RgaNv12FrameTransformer&) = delete;
    RgaNv12FrameTransformer(RgaNv12FrameTransformer&&) = delete;
    RgaNv12FrameTransformer& operator=(RgaNv12FrameTransformer&&) = delete;

    Nv12FrameLease transform(const RgbaFrameView& frame);
    void finish() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vtb
