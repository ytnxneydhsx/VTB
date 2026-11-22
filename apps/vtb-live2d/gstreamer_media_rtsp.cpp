#include "gstreamer_media_session_internal.hpp"

#include <atomic>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include <gst/rtsp/gstrtsptransport.h>

#include "vtb/trace/frame_trace.hpp"

namespace vtb {

#ifdef VTB_FRAME_TRACING
namespace {

constexpr char kRtspTraceMeta[] = "VtbRtspTraceMeta";
constexpr char kEncodedId[] = "encoded_id";
constexpr char kDeliveryId[] = "delivery_id";
constexpr char kRtspQueueFlow[] = "rtsp-h264-queue";
constexpr char kRtspAppsrcFlow[] = "rtsp-appsrc";

std::atomic<guint64> next_rtsp_trace_id{1};

GstStructure* rtsp_trace_fields(GstBuffer* buffer) {
    GstCustomMeta* meta = gst_buffer_get_custom_meta(buffer, kRtspTraceMeta);
    if (meta == nullptr) {
        return nullptr;
    }
    return gst_custom_meta_get_structure(meta);
}

GstStructure* ensure_rtsp_trace_fields(GstBuffer* buffer) {
    static const gchar* tags[] = {nullptr};
    // The default transform preserves these fields across a GstBuffer copy.
    static const GstMetaInfo* const registration = gst_meta_register_custom(
        kRtspTraceMeta, tags, nullptr, nullptr, nullptr);
    if (registration == nullptr) {
        return nullptr;
    }
    GstStructure* fields = rtsp_trace_fields(buffer);
    if (fields == nullptr) {
        GstCustomMeta* meta = gst_buffer_add_custom_meta(buffer, kRtspTraceMeta);
        if (meta != nullptr) {
            fields = gst_custom_meta_get_structure(meta);
        }
    }
    return fields;
}

GstPadProbeReturn trace_rtsp_queue(
    GstPad* pad, GstPadProbeInfo* info, gpointer) {
    if (!TRACE_EVENT_CATEGORY_ENABLED(VTB_FRAME_TRACE_CATEGORY)) {
        return GST_PAD_PROBE_OK;
    }
    GstBuffer* buffer = gst_pad_probe_info_get_buffer(info);
    if (buffer == nullptr) {
        return GST_PAD_PROBE_OK;
    }

    if (gst_pad_get_direction(pad) == GST_PAD_SINK) {
        // The H.264 tee can share this buffer with WebRTC.
        if (!gst_buffer_is_writable(buffer)) {
            GstBuffer* copy = gst_buffer_copy(buffer);
            if (copy == nullptr) {
                return GST_PAD_PROBE_OK;
            }
            gst_pad_probe_info_set_buffer(info, copy);
            buffer = copy;
        }
        GstStructure* fields = ensure_rtsp_trace_fields(buffer);
        if (fields == nullptr) {
            return GST_PAD_PROBE_OK;
        }
        const guint64 encoded_id = next_rtsp_trace_id.fetch_add(1);
        gst_structure_set(fields, kEncodedId, G_TYPE_UINT64, encoded_id, nullptr);
        VTB_FRAME_EVENT(
            "media.rtsp.queue.input", frame_trace_track(),
            perfetto::Flow::ProcessScoped(encoded_id, kRtspQueueFlow),
            "encoded_id", encoded_id);
    } else {
        const GstStructure* fields = rtsp_trace_fields(buffer);
        guint64 encoded_id = 0;
        if (fields != nullptr &&
            gst_structure_get_uint64(fields, kEncodedId, &encoded_id)) {
            VTB_FRAME_EVENT(
                "media.rtsp.queue.output", frame_trace_track(),
                perfetto::TerminatingFlow::ProcessScoped(
                    encoded_id, kRtspQueueFlow),
                "encoded_id", encoded_id);
        }
    }
    return GST_PAD_PROBE_OK;
}

void trace_rtsp_submit(GstBuffer* buffer, bool preroll_request) {
    if (!TRACE_EVENT_CATEGORY_ENABLED(VTB_FRAME_TRACE_CATEGORY)) {
        return;
    }
    GstStructure* fields = ensure_rtsp_trace_fields(buffer);
    if (fields == nullptr) {
        return;
    }
    guint64 encoded_id = 0;
    gst_structure_get_uint64(fields, kEncodedId, &encoded_id);
    // Cached keyframes can be submitted again in a new RTSP media session.
    const guint64 delivery_id = next_rtsp_trace_id.fetch_add(1);
    gst_structure_set(fields, kDeliveryId, G_TYPE_UINT64, delivery_id, nullptr);
    VTB_FRAME_EVENT(
        "media.rtsp.appsrc.submit", frame_trace_track(),
        perfetto::Flow::ProcessScoped(delivery_id, kRtspAppsrcFlow),
        "encoded_id", encoded_id, "delivery_id", delivery_id,
        "preroll", preroll_request);
}

GstPadProbeReturn trace_rtsp_rtp_input(
    GstPad*, GstPadProbeInfo* info, gpointer) {
    if (!TRACE_EVENT_CATEGORY_ENABLED(VTB_FRAME_TRACE_CATEGORY)) {
        return GST_PAD_PROBE_OK;
    }
    GstBuffer* buffer = gst_pad_probe_info_get_buffer(info);
    if (buffer == nullptr) {
        return GST_PAD_PROBE_OK;
    }
    const GstStructure* fields = rtsp_trace_fields(buffer);
    guint64 delivery_id = 0;
    if (fields != nullptr &&
        gst_structure_get_uint64(fields, kDeliveryId, &delivery_id)) {
        guint64 encoded_id = 0;
        gst_structure_get_uint64(fields, kEncodedId, &encoded_id);
        VTB_FRAME_EVENT(
            "media.rtsp.rtp_input", frame_trace_track(),
            perfetto::TerminatingFlow::ProcessScoped(
                delivery_id, kRtspAppsrcFlow),
            "encoded_id", encoded_id, "delivery_id", delivery_id);
    }
    return GST_PAD_PROBE_OK;
}

}  // namespace
#endif

void GStreamerMediaSession::Impl::configure_rtsp_sink(GstElement* upstream) {
    using namespace gstreamer_media_detail;

    GstElement* sink = make_element("appsink", "rtsp-server-input");
    add_elements(pipeline_, {sink});
    GstAppSink* app_sink = GST_APP_SINK(sink);
    gst_app_sink_set_emit_signals(app_sink, TRUE);
    gst_app_sink_set_max_buffers(app_sink, kEncodedQueueCapacity);
    gst_app_sink_set_drop(app_sink, TRUE);
    g_object_set(sink, "sync", FALSE, "enable-last-sample", FALSE, nullptr);

    GstElement* queue = make_element("queue", "rtsp-h264-queue");
    add_elements(pipeline_, {queue});
    configure_queue(queue, kEncodedQueueCapacity);
    link_elements({upstream, queue, sink});
#ifdef VTB_FRAME_TRACING
    add_frame_probe(queue, "sink", trace_rtsp_queue, nullptr);
    add_frame_probe(queue, "src", trace_rtsp_queue, nullptr);
#endif
    g_signal_connect(sink, "new-sample", G_CALLBACK(h264_sample_callback), this);
}

GstFlowReturn GStreamerMediaSession::Impl::h264_sample_callback(
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

void GStreamerMediaSession::Impl::rtsp_media_configure_callback(
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

void GStreamerMediaSession::Impl::rtsp_media_unprepared_callback(
    GstRTSPMedia* media,
    gpointer user_data) noexcept {
    auto& self = *static_cast<Impl*>(user_data);
    self.release_rtsp_shared_stream(media);
}

void GStreamerMediaSession::Impl::rtsp_appsrc_need_data_callback(
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

void GStreamerMediaSession::Impl::start_rtsp_server() {
    using gstreamer_media_detail::parse_rtsp_endpoint;
    using gstreamer_media_detail::RtspEndpoint;

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
        "rtph264pay name=pay0 pt=96 "
        "config-interval=-1 )");
    gst_rtsp_media_factory_set_shared(factory, TRUE);
    gst_rtsp_media_factory_set_stop_on_disconnect(factory, FALSE);
    gst_rtsp_media_factory_set_suspend_mode(
        factory, GST_RTSP_SUSPEND_MODE_NONE);
    gst_rtsp_media_factory_set_eos_shutdown(factory, FALSE);
    gst_rtsp_media_factory_set_profiles(factory, GST_RTSP_PROFILE_AVP);
    gst_rtsp_media_factory_set_protocols(factory, GST_RTSP_LOWER_TRANS_TCP);
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

void GStreamerMediaSession::Impl::configure_rtsp_media(
    GstRTSPMedia* media) {
    const std::unique_ptr<GstElement, decltype(&gst_object_unref)> media_element(
        gst_rtsp_media_get_element(media), gst_object_unref);
    if (media_element == nullptr) {
        throw std::runtime_error("RTSP media has no pipeline element");
    }
#ifdef VTB_FRAME_TRACING
    const std::unique_ptr<GstElement, decltype(&gst_object_unref)> payloader(
        gst_bin_get_by_name_recurse_up(GST_BIN(media_element.get()), "pay0"),
        gst_object_unref);
    if (payloader == nullptr) {
        throw std::runtime_error("RTSP media has no H.264 RTP payloader");
    }
    gstreamer_media_detail::add_frame_probe(
        payloader.get(), "sink", trace_rtsp_rtp_input, nullptr);
#endif
    GstElement* source = gst_bin_get_by_name_recurse_up(
        GST_BIN(media_element.get()), "rtsp-h264-source");
    if (source == nullptr) {
        throw std::runtime_error("RTSP media has no H.264 appsrc");
    }
    gstreamer_media_detail::configure_appsrc(
        source, gstreamer_media_detail::kEncodedQueueCapacity);

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
    rtsp_idr_requested_.store(true);
    std::cout << "VTB_RTSP_MEDIA_CONFIGURED shared=1 reusable=0\n";
}

void GStreamerMediaSession::Impl::release_rtsp_shared_stream(
    GstRTSPMedia* media) noexcept {
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
    rtsp_idr_requested_.store(true);
    std::cout << "VTB_RTSP_MEDIA_RELEASED shared=1 reusable=0\n";
}

GstFlowReturn GStreamerMediaSession::Impl::push_rtsp_buffer(
    GstAppSrc* source,
    GstBuffer* buffer,
    bool keyframe,
    bool preroll_request) {
    std::lock_guard<std::mutex> lock(rtsp_push_mutex_);
    if (preroll_request && !rtsp_waiting_for_keyframe_) {
        return GST_FLOW_OK;
    }
    if (rtsp_waiting_for_keyframe_ && !keyframe) {
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
#ifdef VTB_FRAME_TRACING
    trace_rtsp_submit(output_buffer, preroll_request);
#endif
    const GstFlowReturn result = gst_app_src_push_buffer(
        source, output_buffer);
    if (result == GST_FLOW_OK) {
        if (GST_CLOCK_TIME_IS_VALID(duration) && duration > 0) {
            rtsp_next_timestamp_ += duration;
        } else {
            rtsp_next_timestamp_ += GST_SECOND / render_fps_;
        }
        if (keyframe) {
            rtsp_waiting_for_keyframe_ = false;
        }
        return result;
    }
    if (result == GST_FLOW_FLUSHING || result == GST_FLOW_EOS ||
        result == GST_FLOW_NOT_LINKED) {
        return result;
    }
    throw std::runtime_error(
        std::string("Failed to queue H.264 for shared RTSP media: ") +
        gst_flow_get_name(result));
}

void GStreamerMediaSession::Impl::prime_rtsp_shared_stream(
    GstAppSrc* source) {
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
    rtsp_idr_requested_.store(true);
}

GstFlowReturn GStreamerMediaSession::Impl::publish_h264_sample(
    GstAppSink* sink) {
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
        server_ready = h264_caps_ != nullptr && latest_keyframe_ != nullptr;
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

void GStreamerMediaSession::Impl::cleanup_rtsp_server() noexcept {
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

}  // namespace vtb
