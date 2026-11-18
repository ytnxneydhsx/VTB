#include "gstreamer_media_session_internal.hpp"

#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

#include <gst/rtsp/gstrtspurl.h>

namespace vtb::gstreamer_media_detail {

std::size_t checked_size(std::uint64_t size) {
    if (size == 0 ||
        size > static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max())) {
        throw std::invalid_argument("NV12 DMA-BUF size is invalid");
    }
    return static_cast<std::size_t>(size);
}

void validate_frame(const Nv12FrameView& frame) {
    if (frame.width == 0 || frame.height == 0 ||
        frame.horizontal_stride < frame.width ||
        frame.vertical_stride < frame.height ||
        frame.frames_per_second == 0) {
        throw std::invalid_argument("GStreamer NV12 stream layout is invalid");
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
    gst_util_set_object_arg(G_OBJECT(element), property_name, value_nick);
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

}  // namespace vtb::gstreamer_media_detail
