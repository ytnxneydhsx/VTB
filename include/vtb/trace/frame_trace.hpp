#pragma once

#ifdef VTB_FRAME_TRACING
#include <optional>

#include <perfetto.h>

#define VTB_FRAME_TRACE_CATEGORY "vtb.thread_frame_trace_flow"

PERFETTO_DEFINE_CATEGORIES(
    perfetto::Category(VTB_FRAME_TRACE_CATEGORY).SetDescription(
        "V4L2 input, Live2D tracking and RGBA to NV12 output"));

#define VTB_FRAME_EVENT(...) TRACE_EVENT_INSTANT(VTB_FRAME_TRACE_CATEGORY, __VA_ARGS__)

namespace vtb {
void initialize_frame_tracing();
perfetto::Track frame_trace_track();

namespace detail {

// Perfetto's scoped TRACE_EVENT closes on the default thread track. Both
// markers here must use the custom track under the frame trace group.
class ScopedFrameTraceSlice final {
public:
    template <typename GetTrack, typename Begin>
    ScopedFrameTraceSlice(GetTrack get_track, Begin begin) {
        if (TRACE_EVENT_CATEGORY_ENABLED(VTB_FRAME_TRACE_CATEGORY)) {
            track_.emplace(get_track());
            begin(*track_);
        }
    }

    ~ScopedFrameTraceSlice() {
        if (track_) {
            TRACE_EVENT_END(VTB_FRAME_TRACE_CATEGORY, *track_);
        }
    }

    ScopedFrameTraceSlice(const ScopedFrameTraceSlice&) = delete;
    ScopedFrameTraceSlice& operator=(const ScopedFrameTraceSlice&) = delete;
    ScopedFrameTraceSlice(ScopedFrameTraceSlice&&) = delete;
    ScopedFrameTraceSlice& operator=(ScopedFrameTraceSlice&&) = delete;

private:
    std::optional<perfetto::Track> track_;
};

}  // namespace detail
}  // namespace vtb

#define VTB_FRAME_SLICE(name, trace_track, ...)                                   \
    const ::vtb::detail::ScopedFrameTraceSlice PERFETTO_UID(vtb_frame_slice){     \
        [&]() { return (trace_track); },                                         \
        [&](const ::perfetto::Track& vtb_slice_track) {                          \
            TRACE_EVENT_BEGIN(VTB_FRAME_TRACE_CATEGORY, name, vtb_slice_track,    \
                              ##__VA_ARGS__);                                   \
        }}
#else
#define VTB_FRAME_SLICE(...)
#define VTB_FRAME_EVENT(...)

namespace vtb {
inline void initialize_frame_tracing() {}
}  // namespace vtb
#endif
