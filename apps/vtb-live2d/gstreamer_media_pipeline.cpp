#include "gstreamer_media_session_internal.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <gst/allocators/gstdmabuf.h>
#include <gst/video/video-event.h>
#include <gst/video/video.h>
#include <unistd.h>

namespace vtb {
namespace {

using namespace gstreamer_media_detail;

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

}  // namespace

GstPadProbeReturn GStreamerMediaSession::Impl::h264_output_probe(
    GstPad*,
    GstPadProbeInfo* info,
    gpointer user_data) noexcept {
    auto& self = *static_cast<Impl*>(user_data);
    if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) != 0) {
        self.h264_frames_.fetch_add(1);
    }
    return GST_PAD_PROBE_OK;
}

GstPadProbeReturn GStreamerMediaSession::Impl::mjpeg_output_probe(
    GstPad*,
    GstPadProbeInfo* info,
    gpointer user_data) noexcept {
    auto& self = *static_cast<Impl*>(user_data);
    if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) != 0) {
        self.mjpeg_frames_.fetch_add(1);
    }
    return GST_PAD_PROBE_OK;
}

GstPadProbeReturn GStreamerMediaSession::Impl::h264_upstream_event_probe(
    GstPad*,
    GstPadProbeInfo* info,
    gpointer user_data) noexcept {
    auto& self = *static_cast<Impl*>(user_data);
    if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_UPSTREAM) !=
        0) {
        GstEvent* event = gst_pad_probe_info_get_event(info);
        if (event != nullptr && gst_video_event_is_force_key_unit(event)) {
            self.idr_requests_.fetch_add(1);
        }
    }
    return GST_PAD_PROBE_OK;
}

void GStreamerMediaSession::Impl::raw_queue_overrun(
    GstElement*,
    gpointer user_data) noexcept {
    auto& self = *static_cast<Impl*>(user_data);
    self.raw_dropped_frames_.fetch_add(1);
}

void GStreamerMediaSession::Impl::add_encoder_probe(
    GstElement* encoder,
    GstPadProbeType type,
    GstPadProbeCallback callback) {
    GstPad* source_pad = gst_element_get_static_pad(encoder, "src");
    if (source_pad == nullptr) {
        throw std::runtime_error("GStreamer encoder has no source pad");
    }
    gst_pad_add_probe(source_pad, type, callback, this, nullptr);
    gst_object_unref(source_pad);
}

void GStreamerMediaSession::Impl::configure_appsink(GstElement* sink) {
    GstAppSink* app_sink = GST_APP_SINK(sink);
    gst_app_sink_set_emit_signals(app_sink, TRUE);
    gst_app_sink_set_max_buffers(
        app_sink, gstreamer_media_detail::kEncodedQueueCapacity);
    gst_app_sink_set_drop(app_sink, TRUE);
    g_object_set(
        sink,
        "sync",
        FALSE,
        "enable-last-sample",
        FALSE,
        nullptr);
}

void GStreamerMediaSession::Impl::add_webrtc_output(GstElement* parser) {
    using namespace gstreamer_media_detail;

    const std::string& web_directory = config_.webrtc_web_directory;
    const std::string index_path = web_directory + "/index.html";
    if (!g_path_is_absolute(web_directory.c_str()) ||
        !g_file_test(index_path.c_str(), G_FILE_TEST_IS_REGULAR)) {
        throw std::invalid_argument(
            "WebRTC web_directory must be absolute and contain index.html");
    }

    RtspEndpoint endpoint = parse_rtsp_endpoint(config_.rtsp_url);
    if (endpoint.address.find(':') != std::string::npos &&
        endpoint.address.front() != '[') {
        endpoint.address = "[" + endpoint.address + "]";
    }
    // The bundled gstwebrtc-api page uses the page host and port 8443 for signalling.
    const std::string web_address =
        "http://" + endpoint.address + ":8080/";

    GstElement* sink = make_element("webrtcsink", "live2d-webrtc");
    add_elements(pipeline_, {sink});
    GstElement* tee = make_element("tee", "encoded-h264-tee");
    add_elements(pipeline_, {tee});
    GstElement* rtsp_queue = make_element("queue", "rtsp-h264-queue");
    add_elements(pipeline_, {rtsp_queue});
    GstElement* webrtc_queue = make_element("queue", "webrtc-h264-queue");
    add_elements(pipeline_, {webrtc_queue});

    configure_queue(rtsp_queue, kEncodedQueueCapacity);
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
        "signalling-server-host", endpoint.address.c_str(),
        "signalling-server-port", static_cast<guint>(8443),
        "run-web-server", TRUE,
        "web-server-host-addr", web_address.c_str(),
        "web-server-directory", web_directory.c_str(),
        nullptr);
    gst_structure_free(meta);
    gst_caps_unref(caps);
    // MPP already produced the H.264 stream with a fixed QP.
    set_enum_property(sink, "congestion-control", "disabled");

    link_elements({parser, tee, rtsp_queue, h264_sink_});
    link_elements({tee, webrtc_queue});
    if (!gst_element_link_pads(webrtc_queue, "src", sink, "video_%u")) {
        throw std::runtime_error("Failed to link H.264 to webrtcsink");
    }
}

void GStreamerMediaSession::Impl::build_pipeline() {
    using namespace gstreamer_media_detail;

    pipeline_ = gst_pipeline_new("vtb-media");
    if (pipeline_ == nullptr) {
        throw std::runtime_error("Failed to create GStreamer media pipeline");
    }
    raw_source_ = make_element("appsrc", "nv12-source");
    GstElement* tee = make_element("tee", "nv12-tee");
    GstElement* h264_queue = make_element("queue", "h264-raw-queue");
    h264_encoder_ = make_element("mpph264enc", "h264-encoder");
    GstElement* h264_parser = make_element("h264parse", "h264-parser");
    h264_sink_ = make_element("appsink", "rtsp-server-input");

    configure_appsrc(raw_source_, kRawAppSrcCapacity);
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
    configure_appsink(h264_sink_);
    g_signal_connect(
        h264_queue,
        "overrun",
        G_CALLBACK(raw_queue_overrun),
        this);

    add_elements(
        pipeline_,
        {raw_source_, tee, h264_queue, h264_encoder_, h264_parser,
         h264_sink_});
    link_elements({raw_source_, tee});
    link_elements({tee, h264_queue, h264_encoder_, h264_parser});
    if (config_.webrtc_web_directory.empty()) {
        link_elements({h264_parser, h264_sink_});
    } else {
        add_webrtc_output(h264_parser);
    }
    g_signal_connect(
        h264_sink_,
        "new-sample",
        G_CALLBACK(h264_sample_callback),
        this);
    add_encoder_probe(
        h264_encoder_, GST_PAD_PROBE_TYPE_BUFFER, h264_output_probe);
    add_encoder_probe(
        h264_encoder_,
        GST_PAD_PROBE_TYPE_EVENT_UPSTREAM,
        h264_upstream_event_probe);

    if (config_.uvc_enabled) {
        GstElement* mjpeg_queue = make_element("queue", "mjpeg-raw-queue");
        GstElement* frame_rate = make_element("videorate", "uvc-rate");
        uvc_caps_filter_ = make_element("capsfilter", "uvc-raw-caps");
        mjpeg_encoder_ = make_element("mppjpegenc", "mjpeg-encoder");
        GstElement* jpeg_parser = make_element("jpegparse", "mjpeg-parser");
        GstElement* uvc_output = nullptr;
        if (config_.uvc_link_path.empty()) {
            uvc_output = make_element("uvcsink", "uvc-output");
            set_uvc_device(uvc_output);
        } else {
            uvc_output = make_element("filesink", "uvc-link-output");
            g_object_set(
                uvc_output,
                "location",
                config_.uvc_link_path.c_str(),
                "sync",
                FALSE,
                "async",
                FALSE,
                nullptr);
        }
        configure_queue(mjpeg_queue, kRawQueueCapacity);
        g_object_set(frame_rate, "drop-only", TRUE, nullptr);
        g_object_set(
            mjpeg_encoder_,
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
        g_signal_connect(
            mjpeg_queue,
            "overrun",
            G_CALLBACK(raw_queue_overrun),
            this);
        add_elements(
            pipeline_,
            {mjpeg_queue, frame_rate, uvc_caps_filter_, mjpeg_encoder_,
             jpeg_parser, uvc_output});
        link_elements(
            {tee, mjpeg_queue, frame_rate, uvc_caps_filter_,
             mjpeg_encoder_, jpeg_parser, uvc_output});
        add_encoder_probe(
            mjpeg_encoder_, GST_PAD_PROBE_TYPE_BUFFER, mjpeg_output_probe);
    }
}

void GStreamerMediaSession::Impl::initialize_stream(
    const Nv12FrameView& frame) {
    using namespace gstreamer_media_detail;

    if (stream_initialized_) {
        require_stream_contract(frame);
        return;
    }
    if (config_.uvc_enabled &&
        config_.uvc_fps > frame.frames_per_second) {
        throw std::invalid_argument(
            "UVC FPS exceeds the rendered NV12 frame rate");
    }
    GstCaps* caps = nv12_dma_buf_caps(
        frame.width, frame.height, frame.frames_per_second);
    gst_app_src_set_caps(GST_APP_SRC(raw_source_), caps);
    gst_caps_unref(caps);
    if (frame.frames_per_second >
        static_cast<std::uint32_t>(G_MAXINT / kH264GopSeconds)) {
        throw std::invalid_argument("H.264 frame rate is too large for GOP");
    }
    h264_gop_frames_ =
        static_cast<std::uint64_t>(frame.frames_per_second) *
        kH264GopSeconds;
    g_object_set(
        h264_encoder_,
        "gop",
        static_cast<gint>(h264_gop_frames_),
        nullptr);
    if (config_.uvc_enabled) {
        GstCaps* uvc_caps = nv12_dma_buf_caps(
            frame.width, frame.height, config_.uvc_fps);
        g_object_set(uvc_caps_filter_, "caps", uvc_caps, nullptr);
        gst_caps_unref(uvc_caps);
    }
    width_ = frame.width;
    height_ = frame.height;
    horizontal_stride_ = frame.horizontal_stride;
    vertical_stride_ = frame.vertical_stride;
    render_fps_ = frame.frames_per_second;
    stream_generation_ = frame.stream_generation;
    base_timestamp_ns_ = frame.timestamp_ns;
    set_pipeline_playing(pipeline_, "NV12 MPP RTSP/UVC");
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

void GStreamerMediaSession::Impl::request_h264_idr(GstClockTime timestamp) {
    GstPad* sink_pad = gst_element_get_static_pad(h264_encoder_, "sink");
    if (sink_pad == nullptr) {
        throw std::runtime_error("GStreamer H.264 encoder has no sink pad");
    }
    GstEvent* event = gst_video_event_new_downstream_force_key_unit(
        timestamp,
        GST_CLOCK_TIME_NONE,
        GST_CLOCK_TIME_NONE,
        TRUE,
        key_unit_count_);
    ++key_unit_count_;
    const gboolean accepted = gst_pad_send_event(sink_pad, event);
    gst_object_unref(sink_pad);
    if (!accepted) {
        throw std::runtime_error(
            "GStreamer H.264 encoder rejected a ForceKeyUnit event");
    }
    idr_requests_.fetch_add(1);
}

void GStreamerMediaSession::Impl::push(Nv12FrameLease frame) {
    using namespace gstreamer_media_detail;

    if (finished_) {
        throw std::logic_error("GStreamer media session is finished");
    }
    rethrow_if_failed();
    const Nv12FrameView view = frame.view();
    validate_frame(view);
    initialize_stream(view);
    const bool end_of_stream = view.end_of_stream != 0;

    const int duplicate_fd =
        fcntl(view.dma_buf_fd, F_DUPFD_CLOEXEC, 0);
    if (duplicate_fd < 0) {
        throw std::runtime_error(
            std::string("Failed to duplicate NV12 DMA-BUF: ") +
            std::strerror(errno));
    }
    GstMemory* memory = gst_dmabuf_allocator_alloc(
        dma_buf_allocator_, duplicate_fd, checked_size(view.dma_buf_size));
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

    const std::uint64_t submitted = submitted_frames_.load();
    bool request_idr = client_idr_requested_.exchange(false);
    if (submitted != 0 && submitted % h264_gop_frames_ == 0) {
        request_idr = true;
    }
    if (request_idr) {
        request_h264_idr(GST_BUFFER_PTS(buffer));
    }

    GstFlowReturn result;
    {
        auto scope = raw_push_timer_.scope();
        result = gst_app_src_push_buffer(GST_APP_SRC(raw_source_), buffer);
    }
    if (result != GST_FLOW_OK) {
        throw std::runtime_error(
            std::string("Failed to queue NV12 GstBuffer: ") +
            gst_flow_get_name(result));
    }
    submitted_frames_.fetch_add(1);
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
    g_object_set(child, "device", config_.uvc_device.c_str(), nullptr);
    g_object_unref(child);
}

}  // namespace vtb
