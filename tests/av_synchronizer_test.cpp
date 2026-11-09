#include "vtb/output/av_synchronizer.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using vtb::AvSynchronizer;
using vtb::AvSynchronizerOptions;
using vtb::MediaPacket;

class PacketCollector {
public:
    void receive(const MediaPacket& packet) { packets.push_back(packet); }

    std::vector<MediaPacket> packets;
};

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

MediaPacket packet(const std::string& stream_id, std::uint64_t dts_ns,
                   std::uint64_t sequence, bool keyframe = false,
                   bool discontinuity = false) {
    auto payload = std::make_shared<const std::vector<std::uint8_t>>(
        std::vector<std::uint8_t>{static_cast<std::uint8_t>(sequence)});
    return MediaPacket(stream_id, std::move(payload), dts_ns, dts_ns, 1,
                       sequence, keyframe, discontinuity);
}

AvSynchronizer make_synchronizer(PacketCollector& collector,
                                 AvSynchronizerOptions options = {}) {
    return AvSynchronizer(
        "video", "audio",
        [&collector](const MediaPacket& value) { collector.receive(value); },
        options);
}

void test_processed_audio_holds_video_and_orders_dts() {
    PacketCollector collector;
    AvSynchronizerOptions options;
    options.max_wait = std::chrono::seconds(10);
    AvSynchronizer synchronizer = make_synchronizer(collector, options);

    synchronizer.push(packet("video", 0, 0, true));
    synchronizer.push(packet("video", 40, 1));
    require(collector.packets.empty(), "video escaped before processed audio arrived");

    synchronizer.push(packet("audio", 0, 0));
    require(collector.packets.size() == 2, "initial A/V packets were not released");
    require(collector.packets[0].stream_id() == "video" &&
                collector.packets[0].dts_ns() == 0,
            "equal DTS must release video deterministically first");
    require(collector.packets[1].stream_id() == "audio" &&
                collector.packets[1].dts_ns() == 0,
            "initial audio packet has the wrong position");

    synchronizer.push(packet("audio", 20, 1));
    synchronizer.push(packet("audio", 50, 2));
    require(collector.packets.size() == 4, "DTS interleaving released the wrong count");
    require(collector.packets[2].dts_ns() == 20 &&
                collector.packets[3].dts_ns() == 40,
            "packets were not released in global DTS order");

    synchronizer.finish_stream("video");
    require(collector.packets.size() == 5 &&
                collector.packets.back().dts_ns() == 50,
            "finishing video did not drain the remaining audio");
    synchronizer.finish();
}

void test_forced_release_drops_late_audio() {
    PacketCollector collector;
    AvSynchronizerOptions options;
    options.max_wait = std::chrono::milliseconds(0);
    AvSynchronizer synchronizer = make_synchronizer(collector, options);

    synchronizer.push(packet("video", 100, 1, true));
    synchronizer.push(packet("audio", 50, 1));
    synchronizer.push(packet("audio", 100, 2));

    require(collector.packets.size() == 2, "forced release produced the wrong packet count");
    require(collector.packets[0].stream_id() == "video", "video was not force-released");
    require(collector.packets[1].stream_id() == "audio" &&
                collector.packets[1].discontinuity(),
            "audio recovery did not carry discontinuity");
    const auto status = synchronizer.status();
    require(status.late_audio_packets == 1, "late audio was not counted");
    require(status.forced_releases == 2, "forced releases were not counted");
}

void test_late_video_waits_for_keyframe() {
    PacketCollector collector;
    AvSynchronizerOptions options;
    options.max_wait = std::chrono::milliseconds(0);
    AvSynchronizer synchronizer = make_synchronizer(collector, options);

    synchronizer.push(packet("audio", 100, 1));
    synchronizer.push(packet("video", 50, 1));
    synchronizer.push(packet("video", 110, 2));
    synchronizer.push(packet("video", 120, 3, true));

    require(collector.packets.size() == 2, "late video recovery released prediction frames");
    require(collector.packets[1].stream_id() == "video" &&
                collector.packets[1].keyframe() &&
                collector.packets[1].discontinuity(),
            "video did not recover on a discontinuous keyframe");
    require(synchronizer.status().late_video_packets == 2,
            "late video recovery count is incorrect");
}

void test_queue_capacity_is_bounded() {
    PacketCollector collector;
    AvSynchronizerOptions options;
    options.queue_capacity_per_stream = 2;
    options.max_wait = std::chrono::seconds(10);
    AvSynchronizer synchronizer = make_synchronizer(collector, options);

    synchronizer.push(packet("video", 0, 0, true));
    synchronizer.push(packet("video", 1, 1));
    bool overflow_rejected = false;
    try {
        synchronizer.push(packet("video", 2, 2));
    } catch (const std::overflow_error&) {
        overflow_rejected = true;
    }
    require(overflow_rejected, "full synchronizer queue accepted another packet");
    require(synchronizer.status().rejected_packets == 1,
            "queue rejection was not observable");
    require(synchronizer.status().failed,
            "queue overflow did not fail the synchronization session");

    bool later_push_rejected = false;
    try {
        synchronizer.push(packet("audio", 0, 0));
    } catch (const std::logic_error&) {
        later_push_rejected = true;
    }
    require(later_push_rejected, "failed overflow session accepted another packet");
}

void test_invalid_stream_and_backward_dts_are_rejected() {
    PacketCollector collector;
    AvSynchronizerOptions options;
    options.max_wait = std::chrono::seconds(10);
    AvSynchronizer synchronizer = make_synchronizer(collector, options);

    bool unknown_rejected = false;
    try {
        synchronizer.push(packet("unknown", 0, 0));
    } catch (const std::invalid_argument&) {
        unknown_rejected = true;
    }
    require(unknown_rejected, "unknown stream was accepted");

    synchronizer.push(packet("video", 10, 1, true));
    bool backward_rejected = false;
    try {
        synchronizer.push(packet("video", 9, 2));
    } catch (const std::invalid_argument&) {
        backward_rejected = true;
    }
    require(backward_rejected, "backward per-stream DTS was accepted");
}

void test_sink_failure_is_terminal() {
    AvSynchronizerOptions options;
    options.max_wait = std::chrono::milliseconds(0);
    AvSynchronizer synchronizer(
        "video", "audio",
        [](const MediaPacket&) { throw std::runtime_error("injected sink failure"); },
        options);

    bool failure_observed = false;
    try {
        synchronizer.push(packet("video", 0, 0, true));
    } catch (const std::runtime_error&) {
        failure_observed = true;
    }
    require(failure_observed, "sink failure did not propagate");
    require(synchronizer.status().failed && synchronizer.status().sink_errors == 1,
            "sink failure state was not recorded");

    bool later_push_rejected = false;
    try {
        synchronizer.push(packet("audio", 0, 0));
    } catch (const std::logic_error&) {
        later_push_rejected = true;
    }
    require(later_push_rejected, "failed synchronizer accepted another packet");
}

void test_concurrent_producers_keep_global_dts_order() {
    PacketCollector collector;
    AvSynchronizerOptions options;
    options.queue_capacity_per_stream = 128;
    options.max_wait = std::chrono::seconds(10);
    AvSynchronizer synchronizer = make_synchronizer(collector, options);

    std::thread video_producer([&synchronizer] {
        for (std::uint64_t index = 0; index < 100; ++index) {
            synchronizer.push(packet("video", index * 40, index,
                                     index % 25 == 0));
        }
        synchronizer.finish_stream("video");
    });
    std::thread audio_producer([&synchronizer] {
        for (std::uint64_t index = 0; index < 100; ++index) {
            synchronizer.push(packet("audio", index * 20, index));
        }
        synchronizer.finish_stream("audio");
    });
    video_producer.join();
    audio_producer.join();
    synchronizer.finish();

    require(collector.packets.size() == 200,
            "concurrent producers lost synchronized packets");
    for (std::size_t index = 1; index < collector.packets.size(); ++index) {
        require(collector.packets[index - 1].dts_ns() <=
                    collector.packets[index].dts_ns(),
                "concurrent producers released backward global DTS");
    }
}

}  // namespace

int main() {
    try {
        test_processed_audio_holds_video_and_orders_dts();
        test_forced_release_drops_late_audio();
        test_late_video_waits_for_keyframe();
        test_queue_capacity_is_bounded();
        test_invalid_stream_and_backward_dts_are_rejected();
        test_sink_failure_is_terminal();
        test_concurrent_producers_keep_global_dts_order();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "av-synchronizer-test: " << error.what() << '\n';
        return 1;
    }
}
