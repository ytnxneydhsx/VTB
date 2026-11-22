#pragma once

#ifdef VTB_FRAME_TRACING
#include <perfetto.h>

#define VTB_FRAME_TRACE_CATEGORY "vtb.thread_frame_trace_flow"

PERFETTO_DEFINE_CATEGORIES(
    perfetto::Category(VTB_FRAME_TRACE_CATEGORY).SetDescription(
        "Live2D frame transfer and RTSP output"));

#define VTB_FRAME_EVENT(...) TRACE_EVENT_INSTANT(VTB_FRAME_TRACE_CATEGORY, __VA_ARGS__)
#else
#define VTB_FRAME_EVENT(...)
#endif

namespace vtb {

#ifdef VTB_FRAME_TRACING
void initialize_frame_tracing();
perfetto::Track frame_trace_track();
#else
inline void initialize_frame_tracing() {}
#endif

}  // namespace vtb
