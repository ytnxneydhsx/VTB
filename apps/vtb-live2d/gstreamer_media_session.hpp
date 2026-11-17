#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "vtb/render/nv12_frame_sink.hpp"

namespace vtb {

class GStreamerMediaSessionConfig final {
public:
    std::string rtsp_url;
    std::string webrtc_web_directory;
    bool uvc_enabled = false;
    std::string uvc_device;
    std::uint32_t uvc_fps = 0;
    std::string plugin_path;
    std::string metrics_path;
    std::chrono::milliseconds metrics_report_interval{60000};
};

/**
 * @brief 以 GStreamer 管理 NV12 分支、MPP 编码和 RTSP/WebRTC/UVC 输出。
 *
 * 输入 Nv12FrameLease 被绑定到 DMA-BUF GstMemory。raw appsrc 后的 tee 和有界
 * leaky queue 分别驱动 mpph264enc 与 mppjpegenc。H.264 只向进程内
 * GstRTSPServer 当前处于活动状态的 shared media 提交一次，再由
 * GstRTSPStream 分发给多个客户端；最后一个 RTSP 客户端离开后由 media
 * factory 回收该实例，后续连接创建新的共享实例。启用 WebRTC 时同时供给
 * webrtcsink。
 * MJPEG 交给 UVC 元件。最后一个 GstMemory 引用释放时，
 * Lease 析构并把 RGA 目标槽位归还缓冲池。
 */
class GStreamerMediaSession final {
public:
    explicit GStreamerMediaSession(GStreamerMediaSessionConfig config);
    ~GStreamerMediaSession();

    GStreamerMediaSession(const GStreamerMediaSession&) = delete;
    GStreamerMediaSession& operator=(const GStreamerMediaSession&) = delete;
    GStreamerMediaSession(GStreamerMediaSession&&) = delete;
    GStreamerMediaSession& operator=(GStreamerMediaSession&&) = delete;

    Nv12FrameSink sink() noexcept;
    void rethrow_if_failed() const;
    void finish();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vtb
