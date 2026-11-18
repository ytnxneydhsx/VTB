#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "vtb/render/nv12_frame_sink.hpp"

namespace vtb {

class GStreamerMediaSessionConfig final {
public:
    bool rtsp_enabled = true;
    std::string rtsp_url;
    bool webrtc_enabled = false;
    std::string webrtc_web_directory;
    std::string webrtc_bind_address;
    bool uvc_enabled = false;
    std::string uvc_device;
    std::string uvc_link_path;
    std::uint32_t uvc_fps = 0;
    std::string plugin_path;
    std::string metrics_path;
    std::chrono::milliseconds metrics_report_interval{60000};
};

/**
 * @brief 以 GStreamer 管理 NV12 分支、MPP 编码和 RTSP/WebRTC/UVC 输出。
 *
 * 启用输出时，输入 Nv12FrameLease 被绑定到 DMA-BUF GstMemory。raw appsrc
 * 声明共用的 NV12 caps，帧率取自渲染输出。tee 和有界 leaky queue 分别驱动
 * 已启用的 mpph264enc 与 mppjpegenc。
 * RTSP 和 WebRTC 可独立启用，同时启用时由编码后的 tee 分发同一份 H.264。
 * RTSP 分支只向进程内
 * GstRTSPServer 当前处于活动状态的 shared media 提交一次，再由
 * GstRTSPStream 分发给多个客户端；最后一个 RTSP 客户端离开后由 media
 * factory 回收该实例，后续连接创建新的共享实例。
 * UVC 可通过进程内 uvcsink 或外置 UVC Link 接收 MJPEG。
 * 最后一个 GstMemory 引用释放时，
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
