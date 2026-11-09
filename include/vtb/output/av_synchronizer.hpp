#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "vtb/output/media.hpp"

namespace vtb {

class AvSynchronizerOptions {
public:
    std::size_t queue_capacity_per_stream = 256;
    std::chrono::milliseconds max_wait{500};
};

class AvSynchronizerStatus {
public:
    std::size_t video_queue_depth = 0;
    std::size_t audio_queue_depth = 0;
    std::uint64_t accepted_video_packets = 0;
    std::uint64_t accepted_audio_packets = 0;
    std::uint64_t released_video_packets = 0;
    std::uint64_t released_audio_packets = 0;
    std::uint64_t late_video_packets = 0;
    std::uint64_t late_audio_packets = 0;
    std::uint64_t rejected_packets = 0;
    std::uint64_t forced_releases = 0;
    std::uint64_t sink_errors = 0;
    std::uint64_t last_released_dts_ns = 0;
    bool has_last_released_dts = false;
    bool video_finished = false;
    bool audio_finished = false;
    bool failed = false;
};

// Orders one video stream and one processed-audio stream on their shared
// CLOCK_MONOTONIC timeline before forwarding packets to a nonblocking sink.
// The synchronizer preserves packet timestamps. Capture and processing stages
// remain responsible for assigning timestamps that describe media time.
class AvSynchronizer {
public:
    using PacketSink = std::function<void(const MediaPacket&)>;

    AvSynchronizer(std::string video_stream_id,
                   std::string audio_stream_id,
                   PacketSink sink,
                   AvSynchronizerOptions options = {});
    ~AvSynchronizer();

    AvSynchronizer(const AvSynchronizer&) = delete;
    AvSynchronizer& operator=(const AvSynchronizer&) = delete;
    AvSynchronizer(AvSynchronizer&&) = delete;
    AvSynchronizer& operator=(AvSynchronizer&&) = delete;

    // Thread-safe. Calls the sink serially and may block only for another
    // synchronizer caller or for the sink callback itself.
    void push(const MediaPacket& packet);

    // Marks one producer complete. Remaining packets from the other stream can
    // then be released without waiting for a stream that will never resume.
    void finish_stream(const std::string& stream_id);

    // Marks both producers complete and releases every queued packet in DTS
    // order. Calling finish() more than once is safe.
    void finish();

    AvSynchronizerStatus status() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vtb
