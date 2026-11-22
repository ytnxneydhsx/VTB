#include "vtb/trace/frame_trace.hpp"

#include <chrono>
#include <string>
#include <thread>

PERFETTO_TRACK_EVENT_STATIC_STORAGE();

namespace vtb {
namespace {

const perfetto::NamedTrack& frame_trace_group() {
    static const perfetto::NamedTrack group = [] {
        const perfetto::NamedTrack track("帧链路", 0, perfetto::ProcessTrack::Current());
        perfetto::TrackEvent::SetTrackDescriptor(track, track.Serialize());
        return track;
    }();
    return group;
}

}  // namespace

perfetto::Track frame_trace_track() {
    thread_local const perfetto::Track track = [] {
        const auto thread = perfetto::ThreadTrack::Current();
        const perfetto::Track row(thread.tid, frame_trace_group());
        auto descriptor = row.Serialize();
        std::string name = thread.Serialize().thread().thread_name();
        if (thread.tid == thread.pid) {
            name = "主线程";
        } else if (name.empty()) {
            name = "线程";
        }
        descriptor.set_name(name + " " + std::to_string(thread.tid));
        perfetto::TrackEvent::SetTrackDescriptor(row, descriptor);
        return row;
    }();
    return track;
}

void initialize_frame_tracing() {
    perfetto::TracingInitArgs args;
    args.backends = perfetto::kSystemBackend;
    perfetto::Tracing::Initialize(args);
    perfetto::TrackEvent::Register();

    // Registration is asynchronous. Give the active board session a chance
    // to enable TrackEvent before V4L2 starts, with a bounded startup delay.
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (!TRACE_EVENT_CATEGORY_ENABLED(VTB_FRAME_TRACE_CATEGORY) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    VTB_FRAME_EVENT("trace.producer_ready", frame_trace_track());
}

}  // namespace vtb
