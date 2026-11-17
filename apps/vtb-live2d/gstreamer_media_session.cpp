#include "gstreamer_media_session.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <gst/rtsp/gstrtsptransport.h>
#include <gst/rtsp/gstrtspurl.h>
#include <gst/video/video-event.h>
#include <gst/video/video.h>
#include <unistd.h>

#include "vtb/instrumentation/metrics_jsonl_reporter.hpp"
#include "vtb/instrumentation/timer.hpp"

namespace vtb {
namespace {

constexpr guint kRawQueueCapacity = 2;
constexpr guint64 kRawAppSrcCapacity = 4;
constexpr guint kEncodedQueueCapacity = 8;
constexpr guint kEncoderPendingFrames = 2;
constexpr std::uint32_t kH264GopSeconds = 2;
constexpr GstClockTime kFinishTimeout = 2 * GST_SECOND;

class RtspEndpoint final {
public:
    std::string address;
    std::string service;
    std::string mount_path;
};

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

std::size_t checked_size(std::uint64_t size) {
    if (size == 0 ||
        size > static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max())) {
        throw std::invalid_argument("NV12 DMA-BUF size is invalid");
    }
    return static_cast<std::size_t>(size);
}

void validate_frame(const Nv12FrameView& frame) {
    if (frame.dma_buf_fd < 0 || frame.dma_buf_size == 0 ||
        frame.buffer_id == 0 || frame.stream_generation == 0 ||
        frame.width == 0 || frame.height == 0 ||
        frame.horizontal_stride < frame.width ||
        frame.vertical_stride < frame.height ||
        frame.frames_per_second == 0 || frame.timestamp_ns == 0 ||
        frame.sequence == 0) {
        throw std::invalid_argument("GStreamer NV12 frame is invalid");
    }
    if (frame.end_of_stream != 0 && frame.end_of_stream != 1) {
        throw std::invalid_argument("GStreamer NV12 EOS flag is invalid");
    }
}

GstElement* make_element(const char* factory, const char* name) {
    GstElement* element = gst_element_factory_make(factory, name);
    if (element == nullptr) {
        throw std::runtime_error(
            std::string("Required GStreamer element is unavailable: ") +
            factory);
    }
    return element;
}

void set_enum_property(
    GstElement* element,
    const char* property_name,
    const char* value_nick) {
    GParamSpec* property = g_object_class_find_property(
        G_OBJECT_GET_CLASS(element), property_name);
    if (property == nullptr || !G_IS_PARAM_SPEC_ENUM(property)) {
        throw std::runtime_error(
            std::string("GStreamer enum property is unavailable: ") +
            property_name);
    }
    GEnumClass* enum_class = G_ENUM_CLASS(
        g_type_class_ref(G_PARAM_SPEC_VALUE_TYPE(property)));
    if (enum_class == nullptr) {
        throw std::runtime_error(
            std::string("Failed to inspect GStreamer enum property: ") +
            property_name);
    }
    const GEnumValue* value =
        g_enum_get_value_by_nick(enum_class, value_nick);
    if (value == nullptr) {
        g_type_class_unref(enum_class);
        throw std::runtime_error(
            std::string("Unsupported GStreamer enum value: ") +
            property_name + "=" + value_nick);
    }
    const gint enum_value = value->value;
    g_type_class_unref(enum_class);
    g_object_set(element, property_name, enum_value, nullptr);
}

void add_elements(
    GstElement* pipeline,
    std::initializer_list<GstElement*> elements) {
    for (GstElement* element : elements) {
        if (!gst_bin_add(GST_BIN(pipeline), element)) {
            throw std::runtime_error("Failed to add a GStreamer element");
        }
    }
}

void link_elements(std::initializer_list<GstElement*> elements) {
    GstElement* previous = nullptr;
    for (GstElement* element : elements) {
        if (previous != nullptr && !gst_element_link(previous, element)) {
            throw std::runtime_error("Failed to link GStreamer elements");
        }
        previous = element;
    }
}

void configure_queue(GstElement* queue, guint capacity) {
    g_object_set(
        queue,
        "max-size-buffers",
        capacity,
        "max-size-bytes",
        static_cast<guint>(0),
        "max-size-time",
        static_cast<guint64>(0),
        "leaky",
        static_cast<gint>(2),
        nullptr);
}

void configure_appsrc(GstElement* source, guint64 capacity) {
    g_object_set(
        source,
        "is-live",
        TRUE,
        "format",
        GST_FORMAT_TIME,
        "block",
        FALSE,
        "max-buffers",
        capacity,
        "leaky-type",
        GST_APP_LEAKY_TYPE_DOWNSTREAM,
        nullptr);
}

void set_pipeline_playing(GstElement* pipeline, const char* name) {
    const GstStateChangeReturn result =
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (result == GST_STATE_CHANGE_FAILURE) {
        throw std::runtime_error(
            std::string("Failed to start GStreamer pipeline: ") + name);
    }
}

std::runtime_error pipeline_error(GstMessage* message) {
    GError* error = nullptr;
    gchar* debug = nullptr;
    gst_message_parse_error(message, &error, &debug);
    std::string text = "GStreamer pipeline error";
    if (error != nullptr && error->message != nullptr) {
        text += ": ";
        text += error->message;
    }
    if (debug != nullptr && debug[0] != '\0') {
        text += " (";
        text += debug;
        text += ")";
    }
    if (error != nullptr) {
        g_error_free(error);
    }
    g_free(debug);
    return std::runtime_error(text);
}

GstCaps* nv12_dma_buf_caps(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t frames_per_second) {
    GstCaps* caps = gst_caps_new_simple(
        "video/x-raw",
        "format",
        G_TYPE_STRING,
        "NV12",
        "width",
        G_TYPE_INT,
        static_cast<gint>(width),
        "height",
        G_TYPE_INT,
        static_cast<gint>(height),
        "framerate",
        GST_TYPE_FRACTION,
        static_cast<gint>(frames_per_second),
        1,
        nullptr);
    if (caps == nullptr) {
        throw std::runtime_error("Failed to create GStreamer NV12 caps");
    }
    GstCapsFeatures* features =
        gst_caps_features_new("memory:DMABuf", nullptr);
    gst_caps_set_features(caps, 0, features);
    return caps;
}

GstClockTime relative_timestamp(
    std::uint64_t value,
    std::uint64_t base) noexcept {
    if (value < base) {
        return 0;
    }
    return static_cast<GstClockTime>(value - base);
}

RtspEndpoint parse_rtsp_endpoint(const std::string& value) {
    GstRTSPUrl* parsed_url = nullptr;
    if (gst_rtsp_url_parse(value.c_str(), &parsed_url) != GST_RTSP_OK ||
        parsed_url == nullptr) {
        throw std::invalid_argument("GStreamer RTSP URL is invalid");
    }
    std::unique_ptr<GstRTSPUrl, decltype(&gst_rtsp_url_free)> url(
        parsed_url, gst_rtsp_url_free);
    if (url->host == nullptr || url->host[0] == '\0' ||
        url->abspath == nullptr || url->abspath[0] != '/' ||
        url->abspath[1] == '\0' || url->user != nullptr ||
        url->passwd != nullptr || url->query != nullptr) {
        throw std::invalid_argument(
            "GStreamer RTSP URL must contain only host, port and mount path");
    }
    guint16 port = 0;
    if (gst_rtsp_url_get_port(url.get(), &port) != GST_RTSP_OK || port == 0) {
        throw std::invalid_argument("GStreamer RTSP URL port is invalid");
    }
    RtspEndpoint endpoint;
    endpoint.address = url->host;
    endpoint.service = std::to_string(port);
    endpoint.mount_path = url->abspath;
    return endpoint;
}

}  // namespace

class GStreamerMediaSession::Impl final {
public:
    explicit Impl(GStreamerMediaSessionConfig config)
        : config_(std::move(config)),
          raw_push_timer_(timers_.register_timer("live2d.gstreamer_raw_push")) {
        validate_config();
        configure_plugin_path();
        gst_init(nullptr, nullptr);
        if (!config_.plugin_path.empty()) {
            gst_registry_scan_path(
                gst_registry_get(), config_.plugin_path.c_str());
        }
        dma_buf_allocator_ = gst_dmabuf_allocator_new();
        if (dma_buf_allocator_ == nullptr) {
            throw std::runtime_error("Failed to create DMA-BUF allocator");
        }
        if (!config_.metrics_path.empty()) {
            metrics_reporter_ =
                std::make_unique<instrumentation::MetricsJsonlReporter>(
                    timers_,
                    "vtb-live2d.gstreamer_media",
                    config_.metrics_path,
                    config_.metrics_report_interval);
            metrics_reporter_->start();
        }
        try {
            build_pipeline();
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ~Impl() {
        try {
            finish();
        } catch (...) {
        }
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    Nv12FrameSink sink() noexcept {
        Nv12FrameSink result;
        result.context = this;
        result.submit = submit_callback;
        return result;
    }

    void rethrow_if_failed() const {
        std::exception_ptr failure;
        {
            std::lock_guard<std::mutex> lock(failure_mutex_);
            failure = failure_;
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    void finish() {
        {
            std::lock_guard<std::mutex> lock(finish_mutex_);
            if (finished_) {
                rethrow_if_failed();
                return;
            }
            finished_ = true;
        }

        if (stream_initialized_ && raw_source_ != nullptr &&
            !input_eos_.exchange(true)) {
            gst_app_src_end_of_stream(GST_APP_SRC(raw_source_));
        }
        if (stream_initialized_) {
            wait_for_pipeline_end(pipeline_);
        }

        cleanup();
        finish_metrics();
        std::cout << "VTB_GSTREAMER_MEDIA_RESULT"
                  << " submitted=" << submitted_frames_.load()
                  << " h264=" << h264_frames_.load()
                  << " mjpeg=" << mjpeg_frames_.load()
                  << " rtsp_pushed=" << rtsp_pushed_frames_.load()
                  << " rtsp_no_client=" << rtsp_no_client_frames_.load()
                  << " rtsp_wait_keyframe_dropped="
                  << rtsp_waiting_keyframe_drops_.load()
                  << " raw_dropped=" << raw_dropped_frames_.load()
                  << " idr_requests=" << idr_requests_.load() << "\n";
        rethrow_if_failed();
    }

private:
    static std::int32_t submit_callback(
        void* context,
        Nv12FrameLease* frame) noexcept {
        auto& self = *static_cast<Impl*>(context);
        try {
            if (frame == nullptr) {
                throw std::invalid_argument("NV12 frame lease is null");
            }
            self.push(std::move(*frame));
            return 0;
        } catch (...) {
            self.remember_failure(std::current_exception());
            return -1;
        }
    }

    static GstPadProbeReturn h264_output_probe(
        GstPad*,
        GstPadProbeInfo* info,
        gpointer user_data) noexcept {
        auto& self = *static_cast<Impl*>(user_data);
        if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) != 0) {
            self.h264_frames_.fetch_add(1);
        }
        return GST_PAD_PROBE_OK;
    }

    static GstFlowReturn h264_sample_callback(
        GstAppSink* sink,
        gpointer user_data) noexcept {
        auto& self = *static_cast<Impl*>(user_data);
        try {
            return self.publish_h264_sample(sink);
        } catch (...) {
            self.remember_failure(std::current_exception());
            return GST_FLOW_ERROR;
        }
    }

    static void rtsp_media_configure_callback(
        GstRTSPMediaFactory*,
        GstRTSPMedia* media,
        gpointer user_data) noexcept {
        auto& self = *static_cast<Impl*>(user_data);
        try {
            self.configure_rtsp_media(media);
        } catch (...) {
            self.remember_failure(std::current_exception());
        }
    }

    static void rtsp_media_unprepared_callback(
        GstRTSPMedia* media,
        gpointer user_data) noexcept {
        auto& self = *static_cast<Impl*>(user_data);
        self.release_rtsp_shared_stream(media);
    }

    static void rtsp_appsrc_need_data_callback(
        GstAppSrc* source,
        guint,
        gpointer user_data) noexcept {
        auto& self = *static_cast<Impl*>(user_data);
        try {
            self.prime_rtsp_shared_stream(source);
        } catch (...) {
            self.remember_failure(std::current_exception());
        }
    }

    static GstPadProbeReturn mjpeg_output_probe(
        GstPad*,
        GstPadProbeInfo* info,
        gpointer user_data) noexcept {
        auto& self = *static_cast<Impl*>(user_data);
        if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) != 0) {
            self.mjpeg_frames_.fetch_add(1);
        }
        return GST_PAD_PROBE_OK;
    }

    static GstPadProbeReturn h264_upstream_event_probe(
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

    static void raw_queue_overrun(
        GstElement*,
        gpointer user_data) noexcept {
        auto& self = *static_cast<Impl*>(user_data);
        self.raw_dropped_frames_.fetch_add(1);
    }

    void validate_config() const {
        if (config_.rtsp_url.empty()) {
            throw std::invalid_argument("GStreamer RTSP URL is required");
        }
        if (config_.uvc_enabled &&
            (config_.uvc_device.empty() || config_.uvc_fps == 0)) {
            throw std::invalid_argument(
                "Enabled GStreamer UVC output requires a device and FPS");
        }
        if (config_.metrics_report_interval.count() <= 0) {
            throw std::invalid_argument(
                "GStreamer metrics interval must be positive");
        }
    }

    void configure_plugin_path() const {
        if (config_.plugin_path.empty()) {
            return;
        }
        if (setenv("GST_PLUGIN_PATH", config_.plugin_path.c_str(), 1) != 0 ||
            setenv(
                "GST_PLUGIN_SYSTEM_PATH",
                config_.plugin_path.c_str(),
                1) != 0 ||
            setenv(
                "GST_PLUGIN_PATH_1_0",
                config_.plugin_path.c_str(),
                1) != 0 ||
            setenv(
                "GST_PLUGIN_SYSTEM_PATH_1_0",
                config_.plugin_path.c_str(),
                1) != 0) {
            throw std::runtime_error(
                std::string("Failed to configure GStreamer plugin path: ") +
                std::strerror(errno));
        }
    }

    void add_encoder_probe(
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

    static void configure_appsink(GstElement* sink) {
        GstAppSink* app_sink = GST_APP_SINK(sink);
        gst_app_sink_set_emit_signals(app_sink, TRUE);
        gst_app_sink_set_max_buffers(app_sink, kEncodedQueueCapacity);
        gst_app_sink_set_drop(app_sink, TRUE);
        g_object_set(
            sink,
            "sync",
            FALSE,
            "enable-last-sample",
            FALSE,
            nullptr);
    }

    void add_webrtc_output(GstElement* parser) {
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
        const std::string web_address = "http://" + endpoint.address + ":8080/";

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

    void build_pipeline() {
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
            GstElement* mjpeg_queue =
                make_element("queue", "mjpeg-raw-queue");
            GstElement* frame_rate = make_element("videorate", "uvc-rate");
            uvc_caps_filter_ = make_element("capsfilter", "uvc-raw-caps");
            mjpeg_encoder_ = make_element("mppjpegenc", "mjpeg-encoder");
            GstElement* jpeg_parser =
                make_element("jpegparse", "mjpeg-parser");
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

    void start_rtsp_server() {
        const RtspEndpoint endpoint = parse_rtsp_endpoint(config_.rtsp_url);
        rtsp_context_ = g_main_context_new();
        if (rtsp_context_ == nullptr) {
            throw std::runtime_error("Failed to create RTSP main context");
        }
        rtsp_loop_ = g_main_loop_new(rtsp_context_, FALSE);
        if (rtsp_loop_ == nullptr) {
            throw std::runtime_error("Failed to create RTSP main loop");
        }
        rtsp_server_ = gst_rtsp_server_new();
        if (rtsp_server_ == nullptr) {
            throw std::runtime_error("Failed to create GStreamer RTSP server");
        }
        gst_rtsp_server_set_address(rtsp_server_, endpoint.address.c_str());
        gst_rtsp_server_set_service(rtsp_server_, endpoint.service.c_str());

        GstRTSPMountPoints* mounts =
            gst_rtsp_server_get_mount_points(rtsp_server_);
        if (mounts == nullptr) {
            throw std::runtime_error("GStreamer RTSP server has no mount table");
        }
        GstRTSPMediaFactory* factory = gst_rtsp_media_factory_new();
        if (factory == nullptr) {
            gst_object_unref(mounts);
            throw std::runtime_error("Failed to create RTSP media factory");
        }
        gst_rtsp_media_factory_set_launch(
            factory,
            "( appsrc name=rtsp-h264-source is-live=true format=time "
            "block=false max-buffers=8 leaky-type=downstream ! "
            "queue max-size-buffers=8 max-size-bytes=0 max-size-time=0 "
            "leaky=downstream ! rtph264pay name=pay0 pt=96 "
            "config-interval=-1 )");
        gst_rtsp_media_factory_set_shared(factory, TRUE);
        gst_rtsp_media_factory_set_stop_on_disconnect(factory, FALSE);
        gst_rtsp_media_factory_set_suspend_mode(
            factory, GST_RTSP_SUSPEND_MODE_NONE);
        gst_rtsp_media_factory_set_eos_shutdown(factory, FALSE);
        gst_rtsp_media_factory_set_profiles(factory, GST_RTSP_PROFILE_AVP);
        gst_rtsp_media_factory_set_protocols(
            factory, GST_RTSP_LOWER_TRANS_TCP);
        gst_rtsp_media_factory_set_latency(factory, 0);
        g_signal_connect(
            factory,
            "media-configure",
            G_CALLBACK(rtsp_media_configure_callback),
            this);
        gst_rtsp_mount_points_add_factory(
            mounts, endpoint.mount_path.c_str(), factory);
        gst_object_unref(mounts);

        GError* error = nullptr;
        rtsp_server_source_ =
            gst_rtsp_server_create_source(rtsp_server_, nullptr, &error);
        if (rtsp_server_source_ == nullptr) {
            std::string message = "Failed to bind GStreamer RTSP server";
            if (error != nullptr && error->message != nullptr) {
                message += ": ";
                message += error->message;
            }
            if (error != nullptr) {
                g_error_free(error);
            }
            throw std::runtime_error(message);
        }
        if (g_source_attach(rtsp_server_source_, rtsp_context_) == 0) {
            throw std::runtime_error(
                "Failed to attach GStreamer RTSP server source");
        }
        GMainContext* context = rtsp_context_;
        GMainLoop* loop = rtsp_loop_;
        rtsp_thread_ = std::thread([context, loop]() {
            g_main_context_push_thread_default(context);
            g_main_loop_run(loop);
            g_main_context_pop_thread_default(context);
        });
        std::cout << "VTB_RTSP_SERVER_READY url=" << config_.rtsp_url << "\n";
    }

    void configure_rtsp_media(GstRTSPMedia* media) {
        GstElement* media_element = gst_rtsp_media_get_element(media);
        if (media_element == nullptr) {
            throw std::runtime_error("RTSP media has no pipeline element");
        }
        GstElement* source = gst_bin_get_by_name_recurse_up(
            GST_BIN(media_element), "rtsp-h264-source");
        gst_object_unref(media_element);
        if (source == nullptr) {
            throw std::runtime_error("RTSP media has no H.264 appsrc");
        }
        configure_appsrc(source, kEncodedQueueCapacity);

        GstCaps* caps = nullptr;
        {
            std::lock_guard<std::mutex> lock(rtsp_mutex_);
            if (h264_caps_ == nullptr || latest_keyframe_ == nullptr) {
                gst_object_unref(source);
                throw std::runtime_error(
                    "RTSP media configured before an H.264 keyframe was available");
            }
            caps = gst_caps_ref(h264_caps_);
            if (rtsp_media_ != nullptr || rtsp_source_ != nullptr) {
                gst_caps_unref(caps);
                gst_object_unref(source);
                throw std::logic_error(
                    "Shared RTSP media was configured more than once");
            }
        }
        gst_app_src_set_caps(GST_APP_SRC(source), caps);
        gst_caps_unref(caps);

        GstAppSrcCallbacks callbacks{};
        callbacks.need_data = rtsp_appsrc_need_data_callback;
        gst_app_src_set_callbacks(
            GST_APP_SRC(source), &callbacks, this, nullptr);
        g_signal_connect(
            media,
            "unprepared",
            G_CALLBACK(rtsp_media_unprepared_callback),
            this);
        {
            std::lock_guard<std::mutex> lock(rtsp_mutex_);
            rtsp_media_ = GST_RTSP_MEDIA(gst_object_ref(media));
            rtsp_source_ = source;
        }
        client_idr_requested_.store(true);
        std::cout << "VTB_RTSP_MEDIA_CONFIGURED shared=1 reusable=0\n";
    }

    void release_rtsp_shared_stream(GstRTSPMedia* media) noexcept {
        GstRTSPMedia* retained_media = nullptr;
        GstElement* source = nullptr;
        {
            std::lock_guard<std::mutex> lock(rtsp_mutex_);
            if (rtsp_media_ != media) {
                return;
            }
            retained_media = rtsp_media_;
            rtsp_media_ = nullptr;
            source = rtsp_source_;
            rtsp_source_ = nullptr;
        }
        {
            std::lock_guard<std::mutex> lock(rtsp_push_mutex_);
            rtsp_next_timestamp_ = 0;
            rtsp_waiting_for_keyframe_ = true;
        }
        if (source != nullptr) {
            GstAppSrcCallbacks callbacks{};
            gst_app_src_set_callbacks(
                GST_APP_SRC(source), &callbacks, nullptr, nullptr);
            gst_object_unref(source);
        }
        if (retained_media != nullptr) {
            gst_object_unref(retained_media);
        }
        client_idr_requested_.store(true);
        std::cout << "VTB_RTSP_MEDIA_RELEASED shared=1 reusable=0\n";
    }

    GstFlowReturn push_rtsp_buffer(
        GstAppSrc* source,
        GstBuffer* buffer,
        bool keyframe,
        bool preroll_request) {
        std::lock_guard<std::mutex> lock(rtsp_push_mutex_);
        if (preroll_request && !rtsp_waiting_for_keyframe_) {
            return GST_FLOW_OK;
        }
        if (rtsp_waiting_for_keyframe_ && !keyframe) {
            rtsp_waiting_keyframe_drops_.fetch_add(1);
            return GST_FLOW_OK;
        }

        GstBuffer* output_buffer = gst_buffer_copy(buffer);
        if (output_buffer == nullptr) {
            throw std::runtime_error(
                "Failed to copy H.264 buffer metadata for shared RTSP media");
        }
        GST_BUFFER_PTS(output_buffer) = rtsp_next_timestamp_;
        GST_BUFFER_DTS(output_buffer) = rtsp_next_timestamp_;
        const GstClockTime duration = GST_BUFFER_DURATION(output_buffer);
        const GstFlowReturn result = gst_app_src_push_buffer(
            source, output_buffer);
        if (result == GST_FLOW_OK) {
            if (GST_CLOCK_TIME_IS_VALID(duration) && duration > 0) {
                rtsp_next_timestamp_ += duration;
            } else {
                rtsp_next_timestamp_ += GST_SECOND / 60;
            }
            if (keyframe) {
                rtsp_waiting_for_keyframe_ = false;
            }
            rtsp_pushed_frames_.fetch_add(1);
            return result;
        }
        if (result == GST_FLOW_FLUSHING || result == GST_FLOW_EOS ||
            result == GST_FLOW_NOT_LINKED) {
            rtsp_no_client_frames_.fetch_add(1);
            return result;
        }
        throw std::runtime_error(
            std::string("Failed to queue H.264 for shared RTSP media: ") +
            gst_flow_get_name(result));
    }

    void prime_rtsp_shared_stream(GstAppSrc* source) {
        GstCaps* caps = nullptr;
        GstBuffer* keyframe = nullptr;
        {
            std::lock_guard<std::mutex> lock(rtsp_mutex_);
            if (GST_ELEMENT(source) != rtsp_source_ || h264_caps_ == nullptr ||
                latest_keyframe_ == nullptr) {
                return;
            }
            caps = gst_caps_ref(h264_caps_);
            keyframe = gst_buffer_ref(latest_keyframe_);
        }
        gst_app_src_set_caps(source, caps);
        gst_caps_unref(caps);
        try {
            push_rtsp_buffer(source, keyframe, true, true);
        } catch (...) {
            gst_buffer_unref(keyframe);
            throw;
        }
        gst_buffer_unref(keyframe);
        client_idr_requested_.store(true);
    }

    GstFlowReturn publish_h264_sample(GstAppSink* sink) {
        GstSample* sample = gst_app_sink_pull_sample(sink);
        if (sample == nullptr) {
            return GST_FLOW_EOS;
        }
        GstBuffer* sample_buffer = gst_sample_get_buffer(sample);
        GstCaps* sample_caps = gst_sample_get_caps(sample);
        bool keyframe = false;
        if (sample_buffer != nullptr) {
            keyframe = !GST_BUFFER_FLAG_IS_SET(
                sample_buffer, GST_BUFFER_FLAG_DELTA_UNIT);
        }
        GstElement* target = nullptr;
        GstCaps* target_caps = nullptr;
        bool server_ready = false;
        {
            std::lock_guard<std::mutex> lock(rtsp_mutex_);
            if (sample_caps != nullptr &&
                (h264_caps_ == nullptr ||
                 !gst_caps_is_equal(h264_caps_, sample_caps))) {
                gst_caps_replace(&h264_caps_, sample_caps);
                if (rtsp_source_ != nullptr) {
                    target_caps = gst_caps_ref(h264_caps_);
                }
            }
            if (sample_buffer != nullptr) {
                if (keyframe) {
                    gst_buffer_replace(&latest_keyframe_, sample_buffer);
                }
                if (rtsp_source_ != nullptr) {
                    target = GST_ELEMENT(gst_object_ref(rtsp_source_));
                }
            }
            server_ready =
                h264_caps_ != nullptr && latest_keyframe_ != nullptr;
        }
        if (rtsp_server_ == nullptr && server_ready) {
            start_rtsp_server();
        }
        if (target == nullptr || sample_buffer == nullptr) {
            if (target_caps != nullptr) {
                gst_caps_unref(target_caps);
            }
            if (target != nullptr) {
                gst_object_unref(target);
            }
            rtsp_no_client_frames_.fetch_add(1);
            gst_sample_unref(sample);
            return GST_FLOW_OK;
        }
        if (target_caps != nullptr) {
            gst_app_src_set_caps(GST_APP_SRC(target), target_caps);
            gst_caps_unref(target_caps);
        }
        try {
            push_rtsp_buffer(
                GST_APP_SRC(target), sample_buffer, keyframe, false);
        } catch (...) {
            gst_object_unref(target);
            gst_sample_unref(sample);
            throw;
        }
        gst_object_unref(target);
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    void initialize_stream(const Nv12FrameView& frame) {
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

    void require_stream_contract(const Nv12FrameView& frame) const {
        if (frame.width != width_ || frame.height != height_ ||
            frame.horizontal_stride != horizontal_stride_ ||
            frame.vertical_stride != vertical_stride_ ||
            frame.frames_per_second != render_fps_ ||
            frame.stream_generation != stream_generation_) {
            throw std::invalid_argument(
                "NV12 stream contract changed without GStreamer reconfiguration");
        }
    }

    void request_h264_idr(GstClockTime timestamp) {
        GstPad* sink_pad =
            gst_element_get_static_pad(h264_encoder_, "sink");
        if (sink_pad == nullptr) {
            throw std::runtime_error(
                "GStreamer H.264 encoder has no sink pad");
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

    void push(Nv12FrameLease frame) {
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

    void set_uvc_device(GstElement* sink) const {
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

    void poll_pipeline_error(GstElement* pipeline) {
        if (pipeline == nullptr) {
            return;
        }
        GstBus* bus = gst_element_get_bus(pipeline);
        if (bus == nullptr) {
            return;
        }
        GstMessage* message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
        gst_object_unref(bus);
        if (message == nullptr) {
            return;
        }
        std::runtime_error error = pipeline_error(message);
        gst_message_unref(message);
        throw error;
    }

    void wait_for_pipeline_end(GstElement* pipeline) noexcept {
        if (pipeline == nullptr) {
            return;
        }
        GstBus* bus = gst_element_get_bus(pipeline);
        if (bus == nullptr) {
            return;
        }
        GstMessage* message = gst_bus_timed_pop_filtered(
            bus,
            kFinishTimeout,
            static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        gst_object_unref(bus);
        if (message == nullptr) {
            return;
        }
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            try {
                throw pipeline_error(message);
            } catch (...) {
                remember_failure(std::current_exception());
            }
        }
        gst_message_unref(message);
    }

    void remember_failure(std::exception_ptr failure) noexcept {
        std::lock_guard<std::mutex> lock(failure_mutex_);
        if (!failure_) {
            failure_ = std::move(failure);
        }
    }

    void cleanup_pipeline(GstElement*& pipeline) noexcept {
        if (pipeline == nullptr) {
            return;
        }
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        pipeline = nullptr;
    }

    void cleanup_rtsp_server() noexcept {
        if (rtsp_server_source_ != nullptr) {
            g_source_destroy(rtsp_server_source_);
        }
        if (rtsp_loop_ != nullptr) {
            g_main_loop_quit(rtsp_loop_);
        }
        if (rtsp_thread_.joinable()) {
            rtsp_thread_.join();
        }

        GstRTSPMedia* media = nullptr;
        GstElement* source = nullptr;
        GstCaps* caps = nullptr;
        GstBuffer* keyframe = nullptr;
        {
            std::lock_guard<std::mutex> lock(rtsp_mutex_);
            media = rtsp_media_;
            rtsp_media_ = nullptr;
            source = rtsp_source_;
            rtsp_source_ = nullptr;
            caps = h264_caps_;
            h264_caps_ = nullptr;
            keyframe = latest_keyframe_;
            latest_keyframe_ = nullptr;
        }
        if (source != nullptr) {
            GstAppSrcCallbacks callbacks{};
            gst_app_src_set_callbacks(
                GST_APP_SRC(source), &callbacks, nullptr, nullptr);
        }
        if (media != nullptr) {
            g_signal_handlers_disconnect_by_data(media, this);
            gst_rtsp_media_unprepare(media);
            gst_object_unref(media);
        }
        if (source != nullptr) {
            gst_object_unref(source);
        }
        if (caps != nullptr) {
            gst_caps_unref(caps);
        }
        if (keyframe != nullptr) {
            gst_buffer_unref(keyframe);
        }
        if (rtsp_server_source_ != nullptr) {
            g_source_unref(rtsp_server_source_);
            rtsp_server_source_ = nullptr;
        }
        if (rtsp_server_ != nullptr) {
            gst_object_unref(rtsp_server_);
            rtsp_server_ = nullptr;
        }
        if (rtsp_loop_ != nullptr) {
            g_main_loop_unref(rtsp_loop_);
            rtsp_loop_ = nullptr;
        }
        if (rtsp_context_ != nullptr) {
            g_main_context_unref(rtsp_context_);
            rtsp_context_ = nullptr;
        }
    }

    void cleanup() noexcept {
        cleanup_pipeline(pipeline_);
        cleanup_rtsp_server();
        raw_source_ = nullptr;
        h264_encoder_ = nullptr;
        h264_sink_ = nullptr;
        uvc_caps_filter_ = nullptr;
        mjpeg_encoder_ = nullptr;
        if (dma_buf_allocator_ != nullptr) {
            gst_object_unref(dma_buf_allocator_);
            dma_buf_allocator_ = nullptr;
        }
    }

    void finish_metrics() noexcept {
        if (!metrics_reporter_ || metrics_finished_) {
            return;
        }
        metrics_reporter_->stop();
        metrics_finished_ = true;
        std::cout << "VTB_METRICS_RESULT source="
                  << metrics_reporter_->source()
                  << " path=" << metrics_reporter_->output_path()
                  << " records=" << metrics_reporter_->record_count()
                  << " failed=";
        if (metrics_reporter_->failed()) {
            std::cout << "1 error=" << metrics_reporter_->last_error();
        } else {
            std::cout << "0";
        }
        std::cout << "\n";
    }

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

GStreamerMediaSession::GStreamerMediaSession(
    GStreamerMediaSessionConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

GStreamerMediaSession::~GStreamerMediaSession() = default;

Nv12FrameSink GStreamerMediaSession::sink() noexcept {
    return impl_->sink();
}

void GStreamerMediaSession::rethrow_if_failed() const {
    impl_->rethrow_if_failed();
}

void GStreamerMediaSession::finish() {
    impl_->finish();
}

}  // namespace vtb
