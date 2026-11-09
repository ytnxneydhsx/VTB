#pragma once

#include <chrono>
#include <memory>
#include <string>

#include "vtb/output/encoded_video_sink.hpp"
#include "vtb/output/output.hpp"

namespace vtb {

// Application-owned output. The producer must finish/join before finish() or
// destruction; its callbacks borrow this stable-address object's context.
class MediaOutputSession final {
public:
    MediaOutputSession(
        std::unique_ptr<Output> output,
        std::string metrics_path,
        std::chrono::milliseconds metrics_report_interval,
        std::string output_id,
        std::string stream_id,
        std::string metrics_source,
        std::string write_timer_name);
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
