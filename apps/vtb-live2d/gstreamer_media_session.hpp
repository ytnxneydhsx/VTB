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
    bool uvc_enabled = false;
    std::string uvc_device;
    std::uint32_t uvc_fps = 0;
    std::string plugin_path;
    std::string metrics_path;
    std::chrono::milliseconds metrics_report_interval{60000};
};

/**
 * @brief 以 GStreamer 管理 NV12 分支、MPP 编码和 RTSP/UVC 输出的媒体会话。
 *
 * 输入 Nv12FrameLease 被移动到 GstBuffer。raw appsrc 后的 tee 和有界 leaky
 * queue 分别驱动 H.264 与 MJPEG 分支；每个分支在线程内独占一个 MPP 会话。
 * 最后一个 GstBuffer 引用释放时，Lease 析构并把 RGA 目标槽位归还缓冲池。
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
