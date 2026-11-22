#include "gstreamer_media_session_internal.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <gst/allocators/gstdmabuf.h>
#include <gst/video/video-event.h>
#include <gst/video/video.h>
#include <unistd.h>

#include "vtb/trace/frame_trace.hpp"

namespace vtb {
namespace {

using namespace gstreamer_media_detail;

constexpr guint kUvcJpegQueueCapacity = 10;
constexpr gint64 kUvcSinkTimestampOffset = 40 * GST_MSECOND;
constexpr char kH264RawQueue[] = "h264-raw-queue";
constexpr char kMjpegRawQueue[] = "mjpeg-raw-queue";

class Nv12LeaseHolder final {
public:
    explicit Nv12LeaseHolder(Nv12FrameLease lease)
        : lease_(std::move(lease)) {}
    ~Nv12LeaseHolder() = default;

    Nv12LeaseHolder(const Nv12LeaseHolder&) = delete;
    Nv12LeaseHolder& operator=(const Nv12LeaseHolder&) = delete;
    Nv12LeaseHolder(Nv12LeaseHolder&&) = delete;
    Nv12LeaseHolder& operator=(Nv12LeaseHolder&&) = delete;

private:
    Nv12FrameLease lease_;
};

GQuark lease_holder_quark() {
    static const GQuark value =
        g_quark_from_static_string("vtb-nv12-frame-lease");
    return value;
}

void destroy_lease_holder(gpointer data) {
    delete static_cast<Nv12LeaseHolder*>(data);
}

#ifdef VTB_FRAME_TRACING
constexpr char kAppsrcFlow[] = "nv12-appsrc";

std::optional<std::uint64_t> nv12_probe_sequence(GstPadProbeInfo* info) {
    GstBuffer* buffer = gst_pad_probe_info_get_buffer(info);
    if (buffer == nullptr || !GST_BUFFER_OFFSET_IS_VALID(buffer)) {
        return std::nullopt;
    }
    // push() stores the one-based render sequence as a zero-based buffer offset.
    return GST_BUFFER_OFFSET(buffer) + 1;
}

GstPadProbeReturn trace_appsrc_output(
    GstPad*, GstPadProbeInfo* info, gpointer data) {
    if (!TRACE_EVENT_CATEGORY_ENABLED(VTB_FRAME_TRACE_CATEGORY)) {
        return GST_PAD_PROBE_OK;
    }
    const auto sequence = nv12_probe_sequence(info);
    if (!sequence) {
        return GST_PAD_PROBE_OK;
    }
    const auto& config = *static_cast<const GStreamerMediaSessionConfig*>(data);
    VTB_FRAME_EVENT(
        "media.appsrc.output", frame_trace_track(), "sequence", *sequence,
        [&](perfetto::EventContext context) {
            perfetto::TerminatingFlow::ProcessScoped(*sequence, kAppsrcFlow)(context);
            if (config.rtsp_enabled || config.webrtc_enabled) {
                perfetto::Flow::ProcessScoped(*sequence, kH264RawQueue)(context);
            }
            if (config.uvc_enabled) {
                perfetto::Flow::ProcessScoped(*sequence, kMjpegRawQueue)(context);
            }
        });
    return GST_PAD_PROBE_OK;
}

GstPadProbeReturn trace_raw_queue(
    GstPad* pad, GstPadProbeInfo* info, gpointer data) {
    if (!TRACE_EVENT_CATEGORY_ENABLED(VTB_FRAME_TRACE_CATEGORY)) {
        return GST_PAD_PROBE_OK;
    }
    const auto sequence = nv12_probe_sequence(info);
    if (!sequence) {
        return GST_PAD_PROBE_OK;
    }
    const auto* queue = static_cast<const char*>(data);
    if (gst_pad_get_direction(pad) == GST_PAD_SINK) {
        VTB_FRAME_EVENT(
            "media.raw_queue.input", frame_trace_track(),
            perfetto::Flow::ProcessScoped(*sequence, queue),
            "queue", queue, "sequence", *sequence);
    } else {
        VTB_FRAME_EVENT(
            "media.raw_queue.output", frame_trace_track(),
            perfetto::TerminatingFlow::ProcessScoped(*sequence, queue),
            "queue", queue, "sequence", *sequence);
    }
    return GST_PAD_PROBE_OK;
}

void trace_nv12_queue(GstElement* queue, const char* queue_name) {
    // GStreamer retains the pointer to a static label without modifying it.
    auto* label = const_cast<char*>(queue_name);
    add_frame_probe(queue, "sink", trace_raw_queue, label);
    add_frame_probe(queue, "src", trace_raw_queue, label);
}
#endif

}  // namespace

GstPadProbeReturn GStreamerMediaSession::Impl::h264_input_probe(
    GstPad* pad,
    GstPadProbeInfo* info,
    gpointer user_data) noexcept {
    auto& self = *static_cast<Impl*>(user_data);
    const std::uint64_t received = self.h264_input_frames_++;
    const bool rtsp_request = self.rtsp_idr_requested_.exchange(false);
    const bool periodic_request =
        received != 0 && received % self.h264_gop_frames_ == 0;
    if (!rtsp_request && !periodic_request) {
        return GST_PAD_PROBE_OK;
    }

    try {
        GstBuffer* buffer = gst_pad_probe_info_get_buffer(info);
        GstEvent* event = gst_video_event_new_downstream_force_key_unit(
            GST_BUFFER_PTS(buffer),
            GST_CLOCK_TIME_NONE,
            GST_CLOCK_TIME_NONE,
            TRUE,
            self.key_unit_count_++);
        // 在 H.264 队列线程中先发事件，再将当前帧交给编码器。
        if (!gst_pad_push_event(pad, event)) {
            throw std::runtime_error(
                "GStreamer H.264 encoder rejected a ForceKeyUnit event");
        }
    } catch (...) {
        self.remember_failure(std::current_exception());
    }
    return GST_PAD_PROBE_OK;
}

void GStreamerMediaSession::Impl::configure_webrtc_sink(GstElement* upstream) {
    using namespace gstreamer_media_detail;

    const std::string& web_directory = config_.webrtc_web_directory;
    const std::string index_path = web_directory + "/index.html";
    if (!g_path_is_absolute(web_directory.c_str()) ||
        !g_file_test(index_path.c_str(), G_FILE_TEST_IS_REGULAR)) {
        throw std::invalid_argument(
            "WebRTC web_directory must be absolute and contain index.html");
    }

    std::string address = config_.webrtc_bind_address;
    if (address.find(':') != std::string::npos && address.front() != '[') {
        address = "[" + address + "]";
    }
    // The bundled gstwebrtc-api page uses the page host and port 8443 for signalling.
    const std::string web_address = "http://" + address + ":8080/";

    GstElement* sink = make_element("webrtcsink", "live2d-webrtc");
    add_elements(pipeline_, {sink});
    GstElement* webrtc_queue = make_element("queue", "webrtc-h264-queue");
    add_elements(pipeline_, {webrtc_queue});

    configure_queue(webrtc_queue, kEncodedQueueCapacity);
    GstCaps* caps = gst_caps_new_empty_simple("video/x-h264");
    GstStructure* meta = gst_structure_new(
        "meta", "name", G_TYPE_STRING, "live2d", nullptr);
    g_object_set(
        sink,
        "video-caps", caps,
        "meta", meta,
        "stun-server", static_cast<const gchar*>(nullptr),
        "run-signalling-server", TRUE,
        "signalling-server-host", address.c_str(),
        "signalling-server-port", static_cast<guint>(8443),
        "run-web-server", TRUE,
        "web-server-host-addr", web_address.c_str(),
        "web-server-directory", web_directory.c_str(),
        nullptr);
    gst_structure_free(meta);
    gst_caps_unref(caps);
    // MPP already produced the H.264 stream with a fixed QP.
    set_enum_property(sink, "congestion-control", "disabled");

    link_elements({upstream, webrtc_queue});
    if (!gst_element_link_pads(webrtc_queue, "src", sink, "video_%u")) {
        throw std::runtime_error("Failed to link H.264 to webrtcsink");
    }
}

void GStreamerMediaSession::Impl::build_h264_pipeline(GstElement* nv12_tee) {
    using namespace gstreamer_media_detail;

    GstElement* h264_queue = make_element("queue", kH264RawQueue);
    add_elements(pipeline_, {h264_queue});
    h264_encoder_ = make_element("mpph264enc", "h264-encoder");
    add_elements(pipeline_, {h264_encoder_});
    GstElement* h264_parser = make_element("h264parse", "h264-parser");
    add_elements(pipeline_, {h264_parser});

    configure_queue(h264_queue, kRawQueueCapacity);
    set_enum_property(h264_encoder_, "rc-mode", "fixqp");
    set_enum_property(h264_encoder_, "profile", "high");
    set_enum_property(h264_encoder_, "header-mode", "each-idr");
    g_object_set(
        h264_encoder_,
        "qp-init",
        static_cast<guint>(26),
        "max-pending",
        kEncoderPendingFrames,
        "zero-copy-pkt",
        FALSE,
        nullptr);
    g_object_set(h264_parser, "config-interval", -1, nullptr);

    link_elements({nv12_tee, h264_queue, h264_encoder_, h264_parser});
#ifdef VTB_FRAME_TRACING
    trace_nv12_queue(h264_queue, kH264RawQueue);
#endif
    GstPad* h264_queue_src = gst_element_get_static_pad(h264_queue, "src");
    if (h264_queue_src == nullptr) {
        throw std::runtime_error("GStreamer H.264 queue has no source pad");
    }
    gst_pad_add_probe(
        h264_queue_src, GST_PAD_PROBE_TYPE_BUFFER, h264_input_probe, this, nullptr);
    gst_object_unref(h264_queue_src);

    GstElement* output_source = h264_parser;
    // 两路同时启用时共用一次 H.264 编码，各自的队列隔离下游。
    const bool shared_output = config_.rtsp_enabled && config_.webrtc_enabled;
    if (shared_output) {
        output_source = make_element("tee", "encoded-h264-tee");
        add_elements(pipeline_, {output_source});
        link_elements({h264_parser, output_source});
    }
    if (config_.rtsp_enabled) {
        configure_rtsp_sink(output_source);
    }
    if (config_.webrtc_enabled) {
        configure_webrtc_sink(output_source);
    }
}

void GStreamerMediaSession::Impl::build_mjpeg_pipeline(GstElement* nv12_tee) {
    using namespace gstreamer_media_detail;

    GstElement* mjpeg_queue = make_element("queue", kMjpegRawQueue);
    GstElement* mjpeg_encoder = make_element("mppjpegenc", "mjpeg-encoder");
    GstElement* jpeg_parser = make_element("jpegparse", "mjpeg-parser");
    configure_queue(mjpeg_queue, kRawQueueCapacity);
    g_object_set(
        mjpeg_encoder,
        "q-factor",
        static_cast<guint>(80),
        "qf-min",
        static_cast<guint>(1),
        "qf-max",
        static_cast<guint>(99),
        "max-pending",
        kEncoderPendingFrames,
        "zero-copy-pkt",
        FALSE,
        nullptr);
    add_elements(pipeline_, {mjpeg_queue, mjpeg_encoder, jpeg_parser});
    link_elements({nv12_tee, mjpeg_queue, mjpeg_encoder, jpeg_parser});
#ifdef VTB_FRAME_TRACING
    trace_nv12_queue(mjpeg_queue, kMjpegRawQueue);
#endif
    GstElement* jpeg_queue = make_element("queue", "uvc-mjpeg-output-queue");
    add_elements(pipeline_, {jpeg_queue});
    GstElement* uvc_output = make_element("uvcsink", "uvc-output");
    add_elements(pipeline_, {uvc_output});
    configure_queue(jpeg_queue, kUvcJpegQueueCapacity);
    set_uvc_device(uvc_output);
    link_elements({jpeg_parser, jpeg_queue, uvc_output});
}

void GStreamerMediaSession::Impl::build_pipeline() {
    using namespace gstreamer_media_detail;

    const bool h264_enabled = config_.rtsp_enabled || config_.webrtc_enabled;
    if (!h264_enabled && !config_.uvc_enabled) {
        return;
    }
    pipeline_ = gst_pipeline_new("vtb-media");
    if (pipeline_ == nullptr) {
        throw std::runtime_error("Failed to create GStreamer media pipeline");
    }
    raw_source_ = make_element("appsrc", "nv12-source");
    add_elements(pipeline_, {raw_source_});
    GstElement* tee = make_element("tee", "nv12-tee");
    add_elements(pipeline_, {tee});
    configure_appsrc(raw_source_, kRawAppSrcCapacity);
    link_elements({raw_source_, tee});
#ifdef VTB_FRAME_TRACING
    add_frame_probe(raw_source_, "src", trace_appsrc_output, &config_);
#endif
    if (h264_enabled) {
        build_h264_pipeline(tee);
    }

    if (config_.uvc_enabled) {
        build_mjpeg_pipeline(tee);
    }
}

void GStreamerMediaSession::Impl::initialize_stream(
    const Nv12FrameView& frame) {
    using namespace gstreamer_media_detail;

    if (stream_initialized_) {
        require_stream_contract(frame);
        return;
    }
    validate_frame(frame);
    GstCaps* caps = nv12_dma_buf_caps(
        frame.width, frame.height, frame.frames_per_second);
    gst_app_src_set_caps(GST_APP_SRC(raw_source_), caps);
    gst_caps_unref(caps);
    if (h264_encoder_ != nullptr) {
        if (frame.frames_per_second >
            static_cast<std::uint32_t>(G_MAXINT / kH264GopSeconds)) {
            throw std::invalid_argument("H.264 frame rate is too large for GOP");
        }
        h264_gop_frames_ =
            static_cast<std::uint64_t>(frame.frames_per_second) *
            kH264GopSeconds;
        g_object_set(
            h264_encoder_, "gop", static_cast<gint>(h264_gop_frames_), nullptr);
    }
    width_ = frame.width;
    height_ = frame.height;
    horizontal_stride_ = frame.horizontal_stride;
    vertical_stride_ = frame.vertical_stride;
    render_fps_ = frame.frames_per_second;
    stream_generation_ = frame.stream_generation;
    base_timestamp_ns_ = frame.timestamp_ns;
    set_pipeline_playing(pipeline_, "VTB media output");
    stream_initialized_ = true;
}

void GStreamerMediaSession::Impl::require_stream_contract(
    const Nv12FrameView& frame) const {
    if (frame.width != width_ || frame.height != height_ ||
        frame.horizontal_stride != horizontal_stride_ ||
        frame.vertical_stride != vertical_stride_ ||
        frame.frames_per_second != render_fps_ ||
        frame.stream_generation != stream_generation_) {
        throw std::invalid_argument(
            "NV12 stream contract changed without GStreamer reconfiguration");
    }
}

void GStreamerMediaSession::Impl::push(Nv12FrameLease frame) {
    using namespace gstreamer_media_detail;

    if (finished_) {
        throw std::logic_error("GStreamer media session is finished");
    }
    rethrow_if_failed();
    if (raw_source_ == nullptr) {
        // 所有输出关闭时，帧租约在本次调用结束时归还。
        return;
    }
    const Nv12FrameView view = frame.view();
    initialize_stream(view);
    const bool end_of_stream = view.end_of_stream != 0;

    const std::size_t dma_buf_size = checked_size(view.dma_buf_size);
    const int duplicate_fd =
        fcntl(view.dma_buf_fd, F_DUPFD_CLOEXEC, 0);
    if (duplicate_fd < 0) {
        throw std::runtime_error(
            std::string("Failed to duplicate NV12 DMA-BUF: ") +
            std::strerror(errno));
    }
    GstMemory* memory = gst_dmabuf_allocator_alloc(
        dma_buf_allocator_, duplicate_fd, dma_buf_size);
    if (memory == nullptr) {
        close(duplicate_fd);
        throw std::runtime_error("Failed to wrap NV12 DMA-BUF");
    }
    auto* holder = new Nv12LeaseHolder(std::move(frame));
    gst_mini_object_set_qdata(
        GST_MINI_OBJECT(memory),
        lease_holder_quark(),
        holder,
        destroy_lease_holder);
    GstBuffer* buffer = gst_buffer_new();
    if (buffer == nullptr) {
        gst_memory_unref(memory);
        throw std::runtime_error("Failed to create NV12 GstBuffer");
    }
    gst_buffer_append_memory(buffer, memory);

    gsize offsets[GST_VIDEO_MAX_PLANES]{};
    gint strides[GST_VIDEO_MAX_PLANES]{};
    offsets[1] = static_cast<gsize>(view.horizontal_stride) *
        static_cast<gsize>(view.vertical_stride);
    strides[0] = static_cast<gint>(view.horizontal_stride);
    strides[1] = static_cast<gint>(view.horizontal_stride);
    if (gst_buffer_add_video_meta_full(
            buffer,
            GST_VIDEO_FRAME_FLAG_NONE,
            GST_VIDEO_FORMAT_NV12,
            view.width,
            view.height,
            2,
            offsets,
            strides) == nullptr) {
        gst_buffer_unref(buffer);
        throw std::runtime_error("Failed to attach NV12 video metadata");
    }
    GST_BUFFER_PTS(buffer) =
        relative_timestamp(view.timestamp_ns, base_timestamp_ns_);
    GST_BUFFER_DTS(buffer) = GST_BUFFER_PTS(buffer);
    GST_BUFFER_DURATION(buffer) = GST_SECOND / view.frames_per_second;
    GST_BUFFER_OFFSET(buffer) = view.sequence - 1;

    VTB_FRAME_EVENT(
        "media.appsrc.submit", frame_trace_track(),
        perfetto::Flow::ProcessScoped(view.sequence, kAppsrcFlow),
        "sequence", view.sequence);
    const GstFlowReturn result =
        gst_app_src_push_buffer(GST_APP_SRC(raw_source_), buffer);
    if (result != GST_FLOW_OK) {
        throw std::runtime_error(
            std::string("Failed to queue NV12 GstBuffer: ") +
            gst_flow_get_name(result));
    }
    poll_pipeline_error(pipeline_);
    if (end_of_stream && !input_eos_.exchange(true)) {
        gst_app_src_end_of_stream(GST_APP_SRC(raw_source_));
    }
}

void GStreamerMediaSession::Impl::set_uvc_device(GstElement* sink) const {
    if (!GST_IS_CHILD_PROXY(sink)) {
        throw std::runtime_error("uvcsink does not expose child properties");
    }
    GObject* child = gst_child_proxy_get_child_by_name(
        GST_CHILD_PROXY(sink), "v4l2sink");
    if (child == nullptr) {
        throw std::runtime_error("uvcsink has no v4l2sink child");
    }
    g_object_set(
        child,
        "device",
        config_.uvc_device.c_str(),
        "sync",
        TRUE,
        "ts-offset",
        kUvcSinkTimestampOffset,
        nullptr);
    g_object_unref(child);
}

}  // namespace vtb
