#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>

#include "gstreamer_media_session.hpp"
#include "vtb/instrumentation/metrics_jsonl_reporter.hpp"
#include "vtb/instrumentation/timer.hpp"

namespace vtb {
namespace gstreamer_media_detail {

inline constexpr guint kRawQueueCapacity = 2;
inline constexpr guint64 kRawAppSrcCapacity = 4;
inline constexpr guint kEncodedQueueCapacity = 8;
inline constexpr guint kEncoderPendingFrames = 2;
inline constexpr std::uint32_t kH264GopSeconds = 2;
inline constexpr GstClockTime kFinishTimeout = 2 * GST_SECOND;

class RtspEndpoint final {
public:
    std::string address;
    std::string service;
    std::string mount_path;
};

std::size_t checked_size(std::uint64_t size);
void validate_frame(const Nv12FrameView& frame);
GstElement* make_element(const char* factory, const char* name);
void set_enum_property(
    GstElement* element,
    const char* property_name,
    const char* value_nick);
void add_elements(
    GstElement* pipeline,
    std::initializer_list<GstElement*> elements);
void link_elements(std::initializer_list<GstElement*> elements);
void configure_queue(GstElement* queue, guint capacity);
void configure_appsrc(GstElement* source, guint64 capacity);
void set_pipeline_playing(GstElement* pipeline, const char* name);
std::runtime_error pipeline_error(GstMessage* message);
GstCaps* nv12_dma_buf_caps(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t frames_per_second);
GstClockTime relative_timestamp(
    std::uint64_t value,
    std::uint64_t base) noexcept;
RtspEndpoint parse_rtsp_endpoint(const std::string& value);

}  // namespace gstreamer_media_detail

class GStreamerMediaSession::Impl final {
public:
    explicit Impl(GStreamerMediaSessionConfig config);
    ~Impl();

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    Nv12FrameSink sink() noexcept;
    void rethrow_if_failed() const;
    void finish();

private:
    static std::int32_t submit_callback(
        void* context,
        Nv12FrameLease* frame) noexcept;
    static GstPadProbeReturn h264_output_probe(
        GstPad* pad,
        GstPadProbeInfo* info,
        gpointer user_data) noexcept;
    static GstFlowReturn h264_sample_callback(
        GstAppSink* sink,
        gpointer user_data) noexcept;
    static void rtsp_media_configure_callback(
        GstRTSPMediaFactory* factory,
        GstRTSPMedia* media,
        gpointer user_data) noexcept;
    static void rtsp_media_unprepared_callback(
        GstRTSPMedia* media,
        gpointer user_data) noexcept;
    static void rtsp_appsrc_need_data_callback(
        GstAppSrc* source,
        guint length,
        gpointer user_data) noexcept;
    static GstPadProbeReturn mjpeg_output_probe(
        GstPad* pad,
        GstPadProbeInfo* info,
        gpointer user_data) noexcept;
    static GstPadProbeReturn h264_upstream_event_probe(
        GstPad* pad,
        GstPadProbeInfo* info,
        gpointer user_data) noexcept;
    static void raw_queue_overrun(
        GstElement* queue,
        gpointer user_data) noexcept;

    void validate_config() const;
    void configure_plugin_path() const;
    void add_encoder_probe(
        GstElement* encoder,
        GstPadProbeType type,
        GstPadProbeCallback callback);
    static void configure_appsink(GstElement* sink);
    void add_webrtc_output(GstElement* parser);
    void build_pipeline();

    void start_rtsp_server();
    void configure_rtsp_media(GstRTSPMedia* media);
    void release_rtsp_shared_stream(GstRTSPMedia* media) noexcept;
    GstFlowReturn push_rtsp_buffer(
        GstAppSrc* source,
        GstBuffer* buffer,
        bool keyframe,
        bool preroll_request);
    void prime_rtsp_shared_stream(GstAppSrc* source);
    GstFlowReturn publish_h264_sample(GstAppSink* sink);

    void initialize_stream(const Nv12FrameView& frame);
    void require_stream_contract(const Nv12FrameView& frame) const;
    void request_h264_idr(GstClockTime timestamp);
    void push(Nv12FrameLease frame);
    void set_uvc_device(GstElement* sink) const;

    void poll_pipeline_error(GstElement* pipeline);
    void wait_for_pipeline_end(GstElement* pipeline) noexcept;
    void remember_failure(std::exception_ptr failure) noexcept;
    void cleanup_pipeline(GstElement*& pipeline) noexcept;
    void cleanup_rtsp_server() noexcept;
    void cleanup() noexcept;
    void finish_metrics() noexcept;

    GStreamerMediaSessionConfig config_;
    instrumentation::TimerRegistry timers_;
    instrumentation::TimerHandle raw_push_timer_;
    std::unique_ptr<instrumentation::MetricsJsonlReporter> metrics_reporter_;
    GstAllocator* dma_buf_allocator_ = nullptr;
    GstElement* pipeline_ = nullptr;
    GstElement* raw_source_ = nullptr;
    GstElement* h264_encoder_ = nullptr;
    GstElement* h264_sink_ = nullptr;
    GstElement* uvc_caps_filter_ = nullptr;
    GstElement* mjpeg_encoder_ = nullptr;
    GMainContext* rtsp_context_ = nullptr;
    GMainLoop* rtsp_loop_ = nullptr;
    GstRTSPServer* rtsp_server_ = nullptr;
    GSource* rtsp_server_source_ = nullptr;
    std::thread rtsp_thread_;
    std::mutex rtsp_mutex_;
    std::mutex rtsp_push_mutex_;
    GstRTSPMedia* rtsp_media_ = nullptr;
    GstElement* rtsp_source_ = nullptr;
    GstCaps* h264_caps_ = nullptr;
    GstBuffer* latest_keyframe_ = nullptr;
    mutable std::mutex failure_mutex_;
    std::exception_ptr failure_;
    std::mutex finish_mutex_;
    std::atomic<bool> input_eos_{false};
    std::atomic<bool> client_idr_requested_{false};
    std::atomic<std::uint64_t> submitted_frames_{0};
    std::atomic<std::uint64_t> h264_frames_{0};
    std::atomic<std::uint64_t> mjpeg_frames_{0};
    std::atomic<std::uint64_t> rtsp_pushed_frames_{0};
    std::atomic<std::uint64_t> rtsp_no_client_frames_{0};
    std::atomic<std::uint64_t> rtsp_waiting_keyframe_drops_{0};
    std::atomic<std::uint64_t> raw_dropped_frames_{0};
    std::atomic<std::uint64_t> idr_requests_{0};
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t horizontal_stride_ = 0;
    std::uint32_t vertical_stride_ = 0;
    std::uint32_t render_fps_ = 0;
    std::uint64_t h264_gop_frames_ = 0;
    std::uint64_t stream_generation_ = 0;
    std::uint64_t base_timestamp_ns_ = 0;
    GstClockTime rtsp_next_timestamp_ = 0;
    guint key_unit_count_ = 1;
    bool rtsp_waiting_for_keyframe_ = true;
    bool stream_initialized_ = false;
    bool finished_ = false;
    bool metrics_finished_ = false;
};

}  // namespace vtb
