#pragma once

#include <chrono>
#include <memory>
#include <string>

#include "vtb/output/encoded_video_sink.hpp"
#include "vtb/output/output.hpp"

namespace vtb {

class MediaOutputSessionConfig {
public:
    std::string metrics_path;
    std::chrono::milliseconds metrics_report_interval{60000};
    std::string metrics_source = "vtb-live2d.output";
    std::string rtsp_output_id = "live2d-rtsp";
    std::string uvc_output_id = "live2d-uvc";
    std::string h264_stream_id = "live2d-h264";
    std::string mjpeg_stream_id = "live2d-mjpeg";
};

// Application-owned output. The producer must finish/join before finish() or
// destruction; its callbacks borrow this stable-address object's context.
class MediaOutputSession final {
public:
    MediaOutputSession(
        std::unique_ptr<Output> rtsp_output,
        std::unique_ptr<Output> uvc_output,
        MediaOutputSessionConfig config);
    ~MediaOutputSession();
    MediaOutputSession(const MediaOutputSession&) = delete;
    MediaOutputSession& operator=(const MediaOutputSession&) = delete;
    MediaOutputSession(MediaOutputSession&&) = delete;
    MediaOutputSession& operator=(MediaOutputSession&&) = delete;

    EncodedVideoSink sink() noexcept;
    void rethrow_if_failed() const;
    void finish();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vtb
