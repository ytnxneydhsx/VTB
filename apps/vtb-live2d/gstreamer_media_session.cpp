#include "gstreamer_media_session.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/rtsp/gstrtsptransport.h>
#include <gst/video/video-event.h>
#include <gst/video/video.h>
#include <unistd.h>

#include "mpp_encoder_session.hpp"
#include "vtb/instrumentation/metrics_jsonl_reporter.hpp"
#include "vtb/instrumentation/timer.hpp"

namespace vtb {
namespace {

constexpr guint kRawQueueCapacity = 2;
constexpr guint64 kRawAppSrcCapacity = 4;
constexpr guint64 kEncodedAppSrcCapacity = 8;
constexpr GstClockTime kFinishTimeout = 2 * GST_SECOND;

class Nv12LeaseHolder final {
public:
    explicit Nv12LeaseHolder(Nv12FrameLease lease)
        : lease_(std::move(lease)) {}
    ~Nv12LeaseHolder() = default;

    Nv12LeaseHolder(const Nv12LeaseHolder&) = delete;
    Nv12LeaseHolder& operator=(const Nv12LeaseHolder&) = delete;
    Nv12LeaseHolder(Nv12LeaseHolder&&) = delete;
    Nv12LeaseHolder& operator=(Nv12LeaseHolder&&) = delete;

    const Nv12FrameView& view() const noexcept { return lease_.view(); }

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

GstCaps* raw_nv12_caps(const Nv12FrameView& frame) {
    GstCaps* caps = gst_caps_new_simple(
        "video/x-raw",
        "format",
        G_TYPE_STRING,
        "NV12",
        "width",
        G_TYPE_INT,
        static_cast<gint>(frame.width),
        "height",
        G_TYPE_INT,
        static_cast<gint>(frame.height),
        "framerate",
        GST_TYPE_FRACTION,
        static_cast<gint>(frame.frames_per_second),
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

GstCaps* encoded_caps(
    const char* media_type,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t fps) {
    GstCaps* caps = nullptr;
    if (std::strcmp(media_type, "video/x-h264") == 0) {
        caps = gst_caps_new_simple(
            media_type,
            "stream-format",
            G_TYPE_STRING,
            "byte-stream",
            "alignment",
            G_TYPE_STRING,
            "au",
            "width",
            G_TYPE_INT,
            static_cast<gint>(width),
            "height",
            G_TYPE_INT,
            static_cast<gint>(height),
            "framerate",
            GST_TYPE_FRACTION,
            static_cast<gint>(fps),
            1,
            nullptr);
    } else {
        caps = gst_caps_new_simple(
            media_type,
            "width",
            G_TYPE_INT,
            static_cast<gint>(width),
            "height",
            G_TYPE_INT,
            static_cast<gint>(height),
            "framerate",
            GST_TYPE_FRACTION,
            static_cast<gint>(fps),
            1,
            nullptr);
    }
    if (caps == nullptr) {
        throw std::runtime_error("Failed to create encoded GStreamer caps");
    }
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

}  // namespace

class GStreamerMediaSession::Impl final {
public:
    explicit Impl(GStreamerMediaSessionConfig config)
        : config_(std::move(config)),
          raw_push_timer_(timers_.register_timer("live2d.gstreamer_raw_push")),
          h264_timer_(timers_.register_timer("live2d.mpp_h264")),
          mjpeg_timer_(timers_.register_timer("live2d.mpp_mjpeg")),
          network_push_timer_(
              timers_.register_timer("live2d.gstreamer_network_push")),
          uvc_push_timer_(
              timers_.register_timer("live2d.gstreamer_uvc_push")) {
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
            build_raw_pipeline();
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

        if (raw_source_ != nullptr && !input_eos_.exchange(true)) {
            gst_app_src_end_of_stream(GST_APP_SRC(raw_source_));
        }
        wait_for_pipeline_end(raw_pipeline_);

        if (network_source_ != nullptr) {
            gst_app_src_end_of_stream(GST_APP_SRC(network_source_));
        }
        if (uvc_source_ != nullptr) {
            gst_app_src_end_of_stream(GST_APP_SRC(uvc_source_));
        }
        wait_for_pipeline_end(network_pipeline_);
        wait_for_pipeline_end(uvc_pipeline_);

        cleanup();
        h264_encoder_.shutdown();
        mjpeg_encoder_.shutdown();
        finish_metrics();
        std::cout << "VTB_GSTREAMER_MEDIA_RESULT"
                  << " submitted=" << submitted_frames_.load()
                  << " h264=" << h264_frames_.load()
                  << " mjpeg=" << mjpeg_frames_.load()
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

    static GstFlowReturn h264_sample_callback(
        GstAppSink* sink,
        gpointer user_data) noexcept {
        auto& self = *static_cast<Impl*>(user_data);
        return self.consume_h264_sample(sink);
    }

    static GstFlowReturn mjpeg_sample_callback(
        GstAppSink* sink,
        gpointer user_data) noexcept {
        auto& self = *static_cast<Impl*>(user_data);
        return self.consume_mjpeg_sample(sink);
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
                self.idr_requested_.store(true);
                self.idr_requests_.fetch_add(1);
            }
        }
        return GST_PAD_PROBE_OK;
    }

    static void encoded_queue_overrun(
        GstElement*,
        gpointer user_data) noexcept {
        auto& self = *static_cast<Impl*>(user_data);
        self.idr_requested_.store(true);
        self.idr_requests_.fetch_add(1);
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

    void build_raw_pipeline() {
        raw_pipeline_ = gst_pipeline_new("vtb-raw-media");
        if (raw_pipeline_ == nullptr) {
            throw std::runtime_error("Failed to create raw GStreamer pipeline");
        }
        raw_source_ = make_element("appsrc", "nv12-source");
        GstElement* tee = make_element("tee", "nv12-tee");
        GstElement* h264_queue = make_element("queue", "h264-raw-queue");
        h264_sink_ = make_element("appsink", "h264-mpp-input");
        configure_appsrc(raw_source_, kRawAppSrcCapacity);
        configure_queue(h264_queue, kRawQueueCapacity);
        configure_appsink(h264_sink_);
        g_signal_connect(
            h264_queue,
            "overrun",
            G_CALLBACK(raw_queue_overrun),
            this);

        add_elements(
            raw_pipeline_, {raw_source_, tee, h264_queue, h264_sink_});
        link_elements({raw_source_, tee});
        link_elements({tee, h264_queue, h264_sink_});

        g_signal_connect(
            h264_sink_,
            "new-sample",
            G_CALLBACK(h264_sample_callback),
            this);

        if (config_.uvc_enabled) {
            GstElement* mjpeg_queue =
                make_element("queue", "mjpeg-raw-queue");
            mjpeg_sink_ = make_element("appsink", "mjpeg-mpp-input");
            configure_queue(mjpeg_queue, kRawQueueCapacity);
            configure_appsink(mjpeg_sink_);
            g_signal_connect(
                mjpeg_queue,
                "overrun",
                G_CALLBACK(raw_queue_overrun),
                this);
            add_elements(raw_pipeline_, {mjpeg_queue, mjpeg_sink_});
            link_elements({tee, mjpeg_queue, mjpeg_sink_});
            g_signal_connect(
                mjpeg_sink_,
                "new-sample",
                G_CALLBACK(mjpeg_sample_callback),
                this);
        }
        set_pipeline_playing(raw_pipeline_, "raw NV12 tee");
    }

    static void configure_appsink(GstElement* sink) {
        GstAppSink* app_sink = GST_APP_SINK(sink);
        gst_app_sink_set_emit_signals(app_sink, TRUE);
        gst_app_sink_set_max_buffers(app_sink, kRawQueueCapacity);
        gst_app_sink_set_drop(app_sink, TRUE);
        g_object_set(sink, "sync", FALSE, nullptr);
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
        GstCaps* caps = raw_nv12_caps(frame);
        gst_app_src_set_caps(GST_APP_SRC(raw_source_), caps);
        gst_caps_unref(caps);
        width_ = frame.width;
        height_ = frame.height;
        horizontal_stride_ = frame.horizontal_stride;
        vertical_stride_ = frame.vertical_stride;
        render_fps_ = frame.frames_per_second;
        stream_generation_ = frame.stream_generation;
        base_timestamp_ns_ = frame.timestamp_ns;
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

    void push(Nv12FrameLease frame) {
        if (finished_) {
            throw std::logic_error("GStreamer media session is finished");
        }
        rethrow_if_failed();
        const Nv12FrameView& view = frame.view();
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
        GstBuffer* buffer = gst_buffer_new();
        if (buffer == nullptr) {
            gst_memory_unref(memory);
            throw std::runtime_error("Failed to create NV12 GstBuffer");
        }
        gst_buffer_append_memory(buffer, memory);

        auto* holder = new Nv12LeaseHolder(std::move(frame));
        gst_mini_object_set_qdata(
            GST_MINI_OBJECT(buffer),
            lease_holder_quark(),
            holder,
            destroy_lease_holder);

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
        poll_pipeline_error(raw_pipeline_);
        if (end_of_stream && !input_eos_.exchange(true)) {
            gst_app_src_end_of_stream(GST_APP_SRC(raw_source_));
        }
    }

    const Nv12FrameView& frame_view(GstBuffer* buffer) const {
        auto* holder = static_cast<Nv12LeaseHolder*>(
            gst_mini_object_get_qdata(
                GST_MINI_OBJECT(buffer), lease_holder_quark()));
        if (holder == nullptr) {
            throw std::runtime_error("NV12 GstBuffer lost its frame lease");
        }
        return holder->view();
    }

    GstFlowReturn consume_h264_sample(GstAppSink* sink) noexcept {
        GstSample* sample = gst_app_sink_pull_sample(sink);
        if (sample == nullptr) {
            return GST_FLOW_EOS;
        }
        GstFlowReturn result = GST_FLOW_OK;
        try {
            GstBuffer* input = gst_sample_get_buffer(sample);
            if (input == nullptr) {
                throw std::runtime_error("H.264 branch received no GstBuffer");
            }
            const Nv12FrameView& frame = frame_view(input);
            initialize_h264(frame);
            if (idr_requested_.exchange(false)) {
                h264_encoder_.request_idr();
            }
            MppEncodedPacket packet;
            {
                auto scope = h264_timer_.scope();
                packet = encode(h264_encoder_, frame);
            }
            push_encoded_packet(
                network_source_, packet, frame, frame.frames_per_second, false);
            h264_frames_.fetch_add(1);
            poll_pipeline_error(network_pipeline_);
        } catch (...) {
            remember_failure(std::current_exception());
            result = GST_FLOW_ERROR;
        }
        gst_sample_unref(sample);
        return result;
    }

    GstFlowReturn consume_mjpeg_sample(GstAppSink* sink) noexcept {
        GstSample* sample = gst_app_sink_pull_sample(sink);
        if (sample == nullptr) {
            return GST_FLOW_EOS;
        }
        GstFlowReturn result = GST_FLOW_OK;
        try {
            GstBuffer* input = gst_sample_get_buffer(sample);
            if (input == nullptr) {
                throw std::runtime_error("MJPEG branch received no GstBuffer");
            }
            const Nv12FrameView& frame = frame_view(input);
            if (should_encode_mjpeg(frame.sequence)) {
                initialize_mjpeg(frame);
                MppEncodedPacket packet;
                {
                    auto scope = mjpeg_timer_.scope();
                    packet = encode(mjpeg_encoder_, frame);
                }
                push_encoded_packet(
                    uvc_source_, packet, frame, config_.uvc_fps, true);
                mjpeg_frames_.fetch_add(1);
                poll_pipeline_error(uvc_pipeline_);
            }
        } catch (...) {
            remember_failure(std::current_exception());
            result = GST_FLOW_ERROR;
        }
        gst_sample_unref(sample);
        return result;
    }

    static MppEncodedPacket encode(
        MppEncoderSession& encoder,
        const Nv12FrameView& frame) {
        return encoder.encode(
            frame.dma_buf_fd,
            checked_size(frame.dma_buf_size),
            frame.buffer_id,
            frame.stream_generation,
            frame.timestamp_ns,
            frame.end_of_stream != 0);
    }

    void initialize_h264(const Nv12FrameView& frame) {
        if (h264_initialized_) {
            return;
        }
        h264_encoder_.initialize(
            MppVideoCodec::h264,
            frame.width,
            frame.height,
            frame.horizontal_stride,
            frame.vertical_stride,
            frame.frames_per_second);
        build_network_pipeline(frame);
        h264_initialized_ = true;
    }

    void initialize_mjpeg(const Nv12FrameView& frame) {
        if (mjpeg_initialized_) {
            return;
        }
        mjpeg_encoder_.initialize(
            MppVideoCodec::mjpeg,
            frame.width,
            frame.height,
            frame.horizontal_stride,
            frame.vertical_stride,
            config_.uvc_fps);
        build_uvc_pipeline(frame);
        mjpeg_initialized_ = true;
    }

    void build_network_pipeline(const Nv12FrameView& frame) {
        network_pipeline_ = gst_pipeline_new("vtb-network-output");
        if (network_pipeline_ == nullptr) {
            throw std::runtime_error(
                "Failed to create GStreamer network pipeline");
        }
        network_source_ = make_element("appsrc", "h264-source");
        GstElement* parser = make_element("h264parse", "h264-parser");
        GstElement* queue = make_element("queue", "h264-network-queue");
        GstElement* sink = make_element("rtspclientsink", "rtsp-publisher");
        configure_appsrc(network_source_, kEncodedAppSrcCapacity);
        configure_queue(queue, static_cast<guint>(kEncodedAppSrcCapacity));
        g_object_set(parser, "config-interval", -1, nullptr);
        g_object_set(
            sink,
            "location",
            config_.rtsp_url.c_str(),
            "profiles",
            static_cast<guint>(GST_RTSP_PROFILE_AVP),
            "protocols",
            static_cast<guint>(GST_RTSP_LOWER_TRANS_TCP),
            nullptr);
        g_signal_connect(
            queue, "overrun", G_CALLBACK(encoded_queue_overrun), this);
        add_elements(
            network_pipeline_, {network_source_, parser, queue, sink});
        link_elements({network_source_, parser, queue, sink});
        GstCaps* caps = encoded_caps(
            "video/x-h264",
            frame.width,
            frame.height,
            frame.frames_per_second);
        gst_app_src_set_caps(GST_APP_SRC(network_source_), caps);
        gst_caps_unref(caps);
        GstPad* source_pad = gst_element_get_static_pad(network_source_, "src");
        if (source_pad == nullptr) {
            throw std::runtime_error("H.264 appsrc has no source pad");
        }
        gst_pad_add_probe(
            source_pad,
            GST_PAD_PROBE_TYPE_EVENT_UPSTREAM,
            h264_upstream_event_probe,
            this,
            nullptr);
        gst_object_unref(source_pad);
        set_pipeline_playing(network_pipeline_, "RTSP publisher");
    }

    void build_uvc_pipeline(const Nv12FrameView& frame) {
        uvc_pipeline_ = gst_pipeline_new("vtb-uvc-output");
        if (uvc_pipeline_ == nullptr) {
            throw std::runtime_error("Failed to create GStreamer UVC pipeline");
        }
        uvc_source_ = make_element("appsrc", "mjpeg-source");
        GstElement* parser = make_element("jpegparse", "mjpeg-parser");
        GstElement* queue = make_element("queue", "mjpeg-uvc-queue");
        GstElement* sink = make_element("uvcsink", "uvc-output");
        configure_appsrc(uvc_source_, kEncodedAppSrcCapacity);
        configure_queue(queue, static_cast<guint>(kEncodedAppSrcCapacity));
        set_uvc_device(sink);
        add_elements(uvc_pipeline_, {uvc_source_, parser, queue, sink});
        link_elements({uvc_source_, parser, queue, sink});
        GstCaps* caps = encoded_caps(
            "image/jpeg",
            frame.width,
            frame.height,
            config_.uvc_fps);
        gst_app_src_set_caps(GST_APP_SRC(uvc_source_), caps);
        gst_caps_unref(caps);
        set_pipeline_playing(uvc_pipeline_, "UVC gadget");
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

    void push_encoded_packet(
        GstElement* source,
        const MppEncodedPacket& packet,
        const Nv12FrameView& frame,
        std::uint32_t fps,
        bool uvc) {
        if (source == nullptr || packet.data() == nullptr || packet.size() == 0) {
            throw std::runtime_error("Encoded GStreamer packet is invalid");
        }
        GstBuffer* buffer = gst_buffer_new_allocate(nullptr, packet.size(), nullptr);
        if (buffer == nullptr) {
            throw std::runtime_error("Failed to allocate encoded GstBuffer");
        }
        if (gst_buffer_fill(buffer, 0, packet.data(), packet.size()) !=
            packet.size()) {
            gst_buffer_unref(buffer);
            throw std::runtime_error("Failed to copy encoded MPP packet");
        }
        GST_BUFFER_PTS(buffer) =
            relative_timestamp(packet.pts_ns(), base_timestamp_ns_);
        GST_BUFFER_DTS(buffer) =
            relative_timestamp(packet.dts_ns(), base_timestamp_ns_);
        GST_BUFFER_DURATION(buffer) = GST_SECOND / fps;
        GST_BUFFER_OFFSET(buffer) = frame.sequence - 1;
        if (!packet.keyframe()) {
            GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
        }

        GstFlowReturn result;
        if (uvc) {
            auto scope = uvc_push_timer_.scope();
            result = gst_app_src_push_buffer(GST_APP_SRC(source), buffer);
        } else {
            auto scope = network_push_timer_.scope();
            result = gst_app_src_push_buffer(GST_APP_SRC(source), buffer);
        }
        if (result != GST_FLOW_OK) {
            throw std::runtime_error(
                std::string("Failed to queue encoded GstBuffer: ") +
                gst_flow_get_name(result));
        }
    }

    bool should_encode_mjpeg(std::uint64_t sequence) const noexcept {
        if (!config_.uvc_enabled || config_.uvc_fps == 0) {
            return false;
        }
        if (sequence == 1) {
            return true;
        }
        const std::uint64_t frame_index = sequence - 1;
        const std::uint64_t current =
            frame_index * config_.uvc_fps / render_fps_;
        const std::uint64_t previous =
            (frame_index - 1) * config_.uvc_fps / render_fps_;
        return current > previous;
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

    void cleanup() noexcept {
        cleanup_pipeline(raw_pipeline_);
        cleanup_pipeline(network_pipeline_);
        cleanup_pipeline(uvc_pipeline_);
        raw_source_ = nullptr;
        h264_sink_ = nullptr;
        mjpeg_sink_ = nullptr;
        network_source_ = nullptr;
        uvc_source_ = nullptr;
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
    instrumentation::TimerHandle h264_timer_;
    instrumentation::TimerHandle mjpeg_timer_;
    instrumentation::TimerHandle network_push_timer_;
    instrumentation::TimerHandle uvc_push_timer_;
    std::unique_ptr<instrumentation::MetricsJsonlReporter> metrics_reporter_;
    GstAllocator* dma_buf_allocator_ = nullptr;
    GstElement* raw_pipeline_ = nullptr;
    GstElement* raw_source_ = nullptr;
    GstElement* h264_sink_ = nullptr;
    GstElement* mjpeg_sink_ = nullptr;
    GstElement* network_pipeline_ = nullptr;
    GstElement* network_source_ = nullptr;
    GstElement* uvc_pipeline_ = nullptr;
    GstElement* uvc_source_ = nullptr;
    MppEncoderSession h264_encoder_;
    MppEncoderSession mjpeg_encoder_;
    mutable std::mutex failure_mutex_;
    std::exception_ptr failure_;
    std::mutex finish_mutex_;
    std::atomic<bool> input_eos_{false};
    std::atomic<bool> idr_requested_{false};
    std::atomic<std::uint64_t> submitted_frames_{0};
    std::atomic<std::uint64_t> h264_frames_{0};
    std::atomic<std::uint64_t> mjpeg_frames_{0};
    std::atomic<std::uint64_t> raw_dropped_frames_{0};
    std::atomic<std::uint64_t> idr_requests_{0};
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t horizontal_stride_ = 0;
    std::uint32_t vertical_stride_ = 0;
    std::uint32_t render_fps_ = 0;
    std::uint64_t stream_generation_ = 0;
    std::uint64_t base_timestamp_ns_ = 0;
    bool stream_initialized_ = false;
    bool h264_initialized_ = false;
    bool mjpeg_initialized_ = false;
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
