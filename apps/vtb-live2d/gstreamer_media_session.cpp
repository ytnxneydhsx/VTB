#include "gstreamer_media_session_internal.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

#include <gst/allocators/gstdmabuf.h>

namespace vtb {

GStreamerMediaSession::Impl::Impl(GStreamerMediaSessionConfig config)
    : config_(std::move(config)) {
    validate_config();
    if (config_.webrtc_enabled && config_.webrtc_bind_address.empty()) {
        // 旧配置同时启用两路时，WebRTC 沿用 RTSP URL 的主机地址。
        config_.webrtc_bind_address =
            gstreamer_media_detail::parse_rtsp_endpoint(config_.rtsp_url).address;
    }
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
    try {
        build_pipeline();
    } catch (...) {
        cleanup();
        throw;
    }
}

GStreamerMediaSession::Impl::~Impl() {
    try {
        finish();
    } catch (...) {
    }
}

Nv12FrameSink GStreamerMediaSession::Impl::sink() noexcept {
    Nv12FrameSink result;
    result.context = this;
    result.submit = submit_callback;
    return result;
}

void GStreamerMediaSession::Impl::rethrow_if_failed() const {
    std::exception_ptr failure;
    {
        std::lock_guard<std::mutex> lock(failure_mutex_);
        failure = failure_;
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
}

void GStreamerMediaSession::Impl::finish() {
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
    rethrow_if_failed();
}

std::int32_t GStreamerMediaSession::Impl::submit_callback(
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

void GStreamerMediaSession::Impl::validate_config() const {
    if (config_.rtsp_enabled && config_.rtsp_url.empty()) {
        throw std::invalid_argument("Enabled GStreamer RTSP output requires a URL");
    }
    if (config_.webrtc_enabled && !config_.rtsp_enabled &&
        config_.webrtc_bind_address.empty()) {
        throw std::invalid_argument("WebRTC-only output requires a WebRTC bind address");
    }
    if (config_.uvc_enabled && config_.uvc_device.empty()) {
        throw std::invalid_argument(
            "Enabled GStreamer UVC output requires a device");
    }
}

void GStreamerMediaSession::Impl::configure_plugin_path() const {
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

void GStreamerMediaSession::Impl::poll_pipeline_error(
    GstElement* pipeline) {
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
    std::runtime_error error =
        gstreamer_media_detail::pipeline_error(message);
    gst_message_unref(message);
    throw error;
}

void GStreamerMediaSession::Impl::wait_for_pipeline_end(
    GstElement* pipeline) noexcept {
    if (pipeline == nullptr) {
        return;
    }
    GstBus* bus = gst_element_get_bus(pipeline);
    if (bus == nullptr) {
        return;
    }
    GstMessage* message = gst_bus_timed_pop_filtered(
        bus,
        gstreamer_media_detail::kFinishTimeout,
        static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    gst_object_unref(bus);
    if (message == nullptr) {
        return;
    }
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        try {
            throw gstreamer_media_detail::pipeline_error(message);
        } catch (...) {
            remember_failure(std::current_exception());
        }
    }
    gst_message_unref(message);
}

void GStreamerMediaSession::Impl::remember_failure(
    std::exception_ptr failure) noexcept {
    std::lock_guard<std::mutex> lock(failure_mutex_);
    if (!failure_) {
        failure_ = std::move(failure);
    }
}

void GStreamerMediaSession::Impl::cleanup_pipeline(
    GstElement*& pipeline) noexcept {
    if (pipeline == nullptr) {
        return;
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    pipeline = nullptr;
}

void GStreamerMediaSession::Impl::cleanup() noexcept {
    cleanup_pipeline(pipeline_);
    cleanup_rtsp_server();
    raw_source_ = nullptr;
    h264_encoder_ = nullptr;
    if (dma_buf_allocator_ != nullptr) {
        gst_object_unref(dma_buf_allocator_);
        dma_buf_allocator_ = nullptr;
    }
}

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
