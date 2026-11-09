#include "vtb/output/av_synchronizer.hpp"

#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace vtb {
namespace {

class QueuedPacket {
public:
    QueuedPacket(MediaPacket packet_value,
                 std::chrono::steady_clock::time_point enqueued_value)
        : packet(std::move(packet_value)), enqueued_at(enqueued_value) {}

    MediaPacket packet;
    std::chrono::steady_clock::time_point enqueued_at;
};

}  // namespace

class AvSynchronizer::Impl {
public:
    Impl(std::string video_id, std::string audio_id, PacketSink packet_sink,
         AvSynchronizerOptions synchronizer_options)
        : video_stream_id(std::move(video_id)),
          audio_stream_id(std::move(audio_id)), sink(std::move(packet_sink)),
          options(synchronizer_options) {
        if (video_stream_id.empty() || audio_stream_id.empty() ||
            video_stream_id == audio_stream_id) {
            throw std::invalid_argument(
                "A/V synchronizer requires two distinct non-empty stream IDs");
        }
        if (!sink) {
            throw std::invalid_argument("A/V synchronizer requires a packet sink");
        }
        if (options.queue_capacity_per_stream == 0 ||
            options.max_wait.count() < 0) {
            throw std::invalid_argument("invalid A/V synchronizer options");
        }
    }

    ~Impl() = default;

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void push(const MediaPacket& packet) {
        std::lock_guard<std::mutex> dispatch_lock(dispatch_mutex);
        std::vector<MediaPacket> ready;
        bool rejected_for_capacity = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            ensure_active();
            const bool video = stream_is_video(packet.stream_id());
            validate_input_order(packet, video);

            const auto now = std::chrono::steady_clock::now();
            collect_ready(now, ready);

            if (packet_is_late(packet)) {
                drop_late(packet, video);
            } else if (video && wait_video_keyframe && !packet.keyframe()) {
                ++late_video_packets;
            } else {
                std::deque<QueuedPacket>& queue = queue_for(video);
                if (queue.size() >= options.queue_capacity_per_stream) {
                    ++rejected_packets;
                    failed = true;
                    rejected_for_capacity = true;
                } else {
                    if (video && packet.keyframe()) {
                        wait_video_keyframe = false;
                    }
                    queue.emplace_back(packet, now);
                    increment_accepted(video);
                    collect_ready(now, ready);
                }
            }
        }
        dispatch(ready);
        if (rejected_for_capacity) {
            throw std::overflow_error("A/V synchronizer stream queue is full");
        }
    }

    void finish_stream(const std::string& stream_id) {
        std::lock_guard<std::mutex> dispatch_lock(dispatch_mutex);
        std::vector<MediaPacket> ready;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            ensure_active();
            const bool video = stream_is_video(stream_id);
            if (video) {
                video_finished = true;
            } else {
                audio_finished = true;
            }
            collect_ready(std::chrono::steady_clock::now(), ready);
        }
        dispatch(ready);
    }

    void finish() {
        std::lock_guard<std::mutex> dispatch_lock(dispatch_mutex);
        std::vector<MediaPacket> ready;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (failed) {
                throw std::logic_error("A/V synchronizer has failed");
            }
            video_finished = true;
            audio_finished = true;
            collect_ready(std::chrono::steady_clock::now(), ready);
        }
        dispatch(ready);
    }

    AvSynchronizerStatus current_status() const {
        std::lock_guard<std::mutex> lock(state_mutex);
        AvSynchronizerStatus result;
        result.video_queue_depth = video_queue.size();
        result.audio_queue_depth = audio_queue.size();
        result.accepted_video_packets = accepted_video_packets;
        result.accepted_audio_packets = accepted_audio_packets;
        result.released_video_packets = released_video_packets;
        result.released_audio_packets = released_audio_packets;
        result.late_video_packets = late_video_packets;
        result.late_audio_packets = late_audio_packets;
        result.rejected_packets = rejected_packets;
        result.forced_releases = forced_releases;
        result.sink_errors = sink_errors;
        result.video_finished = video_finished;
        result.audio_finished = audio_finished;
        result.failed = failed;
        if (last_released_dts_ns.has_value()) {
            result.last_released_dts_ns = *last_released_dts_ns;
            result.has_last_released_dts = true;
        }
        return result;
    }

private:
    bool stream_is_video(const std::string& stream_id) const {
        if (stream_id == video_stream_id) {
            return true;
        }
        if (stream_id == audio_stream_id) {
            return false;
        }
        throw std::invalid_argument(
            "packet does not belong to this A/V synchronizer");
    }

    void ensure_active() const {
        if (failed) {
            throw std::logic_error("A/V synchronizer has failed");
        }
    }

    void validate_input_order(const MediaPacket& packet, bool video) {
        if ((video && video_finished) || (!video && audio_finished)) {
            throw std::logic_error("cannot push a finished A/V stream");
        }
        std::optional<std::uint64_t>& last_dts = last_received_dts_for(video);
        if (last_dts.has_value() && packet.dts_ns() < *last_dts) {
            throw std::invalid_argument(
                "packet DTS moved backwards within one A/V stream");
        }
        last_dts = packet.dts_ns();
    }

    bool packet_is_late(const MediaPacket& packet) const {
        return last_released_dts_ns.has_value() &&
               packet.dts_ns() < *last_released_dts_ns;
    }

    void drop_late(const MediaPacket&, bool video) {
        if (video) {
            ++late_video_packets;
            wait_video_keyframe = true;
            pending_video_discontinuity = true;
            return;
        }
        ++late_audio_packets;
        pending_audio_discontinuity = true;
    }

    std::deque<QueuedPacket>& queue_for(bool video) {
        if (video) {
            return video_queue;
        }
        return audio_queue;
    }

    std::optional<std::uint64_t>& last_received_dts_for(bool video) {
        if (video) {
            return last_received_video_dts_ns;
        }
        return last_received_audio_dts_ns;
    }

    void increment_accepted(bool video) {
        if (video) {
            ++accepted_video_packets;
        } else {
            ++accepted_audio_packets;
        }
    }

    void collect_ready(std::chrono::steady_clock::time_point now,
                       std::vector<MediaPacket>& ready) {
        while (true) {
            discard_queued_late_packets();
            if (!video_queue.empty() && !audio_queue.empty()) {
                if (video_queue.front().packet.dts_ns() <=
                    audio_queue.front().packet.dts_ns()) {
                    release_front(true, false, ready);
                } else {
                    release_front(false, false, ready);
                }
                continue;
            }
            if (!video_queue.empty()) {
                if (audio_finished) {
                    release_front(true, false, ready);
                    continue;
                }
                if (wait_expired(video_queue.front(), now)) {
                    release_front(true, true, ready);
                    continue;
                }
            }
            if (!audio_queue.empty()) {
                if (video_finished) {
                    release_front(false, false, ready);
                    continue;
                }
                if (wait_expired(audio_queue.front(), now)) {
                    release_front(false, true, ready);
                    continue;
                }
            }
            break;
        }
    }

    bool wait_expired(const QueuedPacket& packet,
                      std::chrono::steady_clock::time_point now) const {
        return now - packet.enqueued_at >= options.max_wait;
    }

    void discard_queued_late_packets() {
        if (!last_released_dts_ns.has_value()) {
            return;
        }
        while (!video_queue.empty() &&
               video_queue.front().packet.dts_ns() < *last_released_dts_ns) {
            video_queue.pop_front();
            ++late_video_packets;
            wait_video_keyframe = true;
            pending_video_discontinuity = true;
        }
        while (!audio_queue.empty() &&
               audio_queue.front().packet.dts_ns() < *last_released_dts_ns) {
            audio_queue.pop_front();
            ++late_audio_packets;
            pending_audio_discontinuity = true;
        }
        while (wait_video_keyframe && !video_queue.empty() &&
               !video_queue.front().packet.keyframe()) {
            video_queue.pop_front();
            ++late_video_packets;
        }
        if (wait_video_keyframe && !video_queue.empty() &&
            video_queue.front().packet.keyframe()) {
            wait_video_keyframe = false;
        }
    }

    void release_front(bool video, bool forced,
                       std::vector<MediaPacket>& ready) {
        std::deque<QueuedPacket>& queue = queue_for(video);
        MediaPacket packet = std::move(queue.front().packet);
        queue.pop_front();
        bool& pending_discontinuity = pending_discontinuity_for(video);
        if (pending_discontinuity && !packet.discontinuity()) {
            packet = std::move(packet).with_discontinuity();
        }
        pending_discontinuity = false;
        if (last_released_dts_ns.has_value() &&
            packet.dts_ns() < *last_released_dts_ns) {
            throw std::logic_error("A/V synchronizer released backward DTS");
        }
        last_released_dts_ns = packet.dts_ns();
        if (video) {
            ++released_video_packets;
        } else {
            ++released_audio_packets;
        }
        if (forced) {
            ++forced_releases;
        }
        ready.push_back(std::move(packet));
    }

    bool& pending_discontinuity_for(bool video) {
        if (video) {
            return pending_video_discontinuity;
        }
        return pending_audio_discontinuity;
    }

    void dispatch(const std::vector<MediaPacket>& ready) {
        try {
            for (const MediaPacket& packet : ready) {
                sink(packet);
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(state_mutex);
            ++sink_errors;
            failed = true;
            throw;
        }
    }

    std::string video_stream_id;
    std::string audio_stream_id;
    PacketSink sink;
    AvSynchronizerOptions options;
    mutable std::mutex state_mutex;
    std::mutex dispatch_mutex;
    std::deque<QueuedPacket> video_queue;
    std::deque<QueuedPacket> audio_queue;
    std::optional<std::uint64_t> last_received_video_dts_ns;
    std::optional<std::uint64_t> last_received_audio_dts_ns;
    std::optional<std::uint64_t> last_released_dts_ns;
    bool video_finished = false;
    bool audio_finished = false;
    bool wait_video_keyframe = false;
    bool pending_video_discontinuity = false;
    bool pending_audio_discontinuity = false;
    bool failed = false;
    std::uint64_t accepted_video_packets = 0;
    std::uint64_t accepted_audio_packets = 0;
    std::uint64_t released_video_packets = 0;
    std::uint64_t released_audio_packets = 0;
    std::uint64_t late_video_packets = 0;
    std::uint64_t late_audio_packets = 0;
    std::uint64_t rejected_packets = 0;
    std::uint64_t forced_releases = 0;
    std::uint64_t sink_errors = 0;
};

AvSynchronizer::AvSynchronizer(std::string video_stream_id,
                               std::string audio_stream_id,
                               PacketSink sink,
                               AvSynchronizerOptions options)
    : impl_(std::make_unique<Impl>(std::move(video_stream_id),
                                   std::move(audio_stream_id),
                                   std::move(sink), options)) {}

AvSynchronizer::~AvSynchronizer() = default;

void AvSynchronizer::push(const MediaPacket& packet) { impl_->push(packet); }

void AvSynchronizer::finish_stream(const std::string& stream_id) {
    impl_->finish_stream(stream_id);
}

void AvSynchronizer::finish() { impl_->finish(); }

AvSynchronizerStatus AvSynchronizer::status() const {
    return impl_->current_status();
}

}  // namespace vtb
