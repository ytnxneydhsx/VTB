#pragma once

#include <stdint.h>

#if defined(_WIN32)
#define OPENVT_LIBGODOT_API __declspec(dllexport)
#else
#define OPENVT_LIBGODOT_API __attribute__((visibility("default"), used))
#endif

extern "C" {

OPENVT_LIBGODOT_API int openvt_libgodot_start(void *p_instance);
OPENVT_LIBGODOT_API int openvt_libgodot_is_started(void *p_instance);
OPENVT_LIBGODOT_API int openvt_libgodot_iteration(void *p_instance);
OPENVT_LIBGODOT_API int openvt_libgodot_application_initialize(void);
OPENVT_LIBGODOT_API int openvt_libgodot_application_prepare_frame(uint64_t p_monotonic_timestamp_ns);
OPENVT_LIBGODOT_API int openvt_libgodot_application_complete_frame(void);
OPENVT_LIBGODOT_API int openvt_libgodot_application_should_stop(void);
OPENVT_LIBGODOT_API uint64_t openvt_libgodot_application_completed_frames(void);
OPENVT_LIBGODOT_API int openvt_libgodot_application_shutdown(void);
OPENVT_LIBGODOT_API const char *openvt_libgodot_application_last_error(void);
OPENVT_LIBGODOT_API void openvt_libgodot_bridge_force_link(void);

}
