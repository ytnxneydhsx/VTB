#pragma once

#include <stdint.h>
#include "vtb/output/encoded_video_sink.hpp"

inline constexpr uint32_t OPENVT_LIBGODOT_ABI_VERSION = 3;

#if defined(_WIN32)
#define OPENVT_LIBGODOT_API __declspec(dllexport)
#else
#define OPENVT_LIBGODOT_API __attribute__((visibility("default"), used))
#endif

class OpenVTLive2DFrameInput {
public:
	float angle_x;
	float angle_y;
	float angle_z;
	float body_angle_x;
	float body_angle_y;
	float body_angle_z;
	float left_eye_open;
	float right_eye_open;
	float mouth_open;
	float mouth_form;
	float view_zoom;
	float view_offset_x;
	float view_offset_y;
	uint64_t produced_timestamp_ns;
	uint64_t snapshot_generation;
};

class OpenVTEmbeddedApplicationConfig {
public:
	const char *live2d_model;
	uint32_t abi_version;
	uint32_t config_size;
	vtb::EncodedVideoSink output_sink;
	uint32_t output_width;
	uint32_t output_height;
	uint32_t output_fps;
	uint32_t total_frames;
	const char *metrics_jsonl_path;
	uint32_t metrics_report_interval_ms;
};

extern "C" {

OPENVT_LIBGODOT_API uint32_t openvt_libgodot_bridge_abi_version(void);

OPENVT_LIBGODOT_API int openvt_libgodot_start(void *p_instance);
OPENVT_LIBGODOT_API int openvt_libgodot_is_started(void *p_instance);
OPENVT_LIBGODOT_API int openvt_libgodot_iteration(void *p_instance);
OPENVT_LIBGODOT_API int openvt_libgodot_application_initialize_v3(
		const OpenVTEmbeddedApplicationConfig *p_config);
OPENVT_LIBGODOT_API int openvt_libgodot_application_submit_live2d_frame(
		const OpenVTLive2DFrameInput *p_input);
OPENVT_LIBGODOT_API int openvt_libgodot_application_prepare_frame(uint64_t p_monotonic_timestamp_ns);
OPENVT_LIBGODOT_API int openvt_libgodot_application_complete_frame(void);
OPENVT_LIBGODOT_API int openvt_libgodot_application_should_stop(void);
OPENVT_LIBGODOT_API uint64_t openvt_libgodot_application_completed_frames(void);
OPENVT_LIBGODOT_API int openvt_libgodot_application_shutdown(void);
OPENVT_LIBGODOT_API const char *openvt_libgodot_application_last_error(void);
OPENVT_LIBGODOT_API void openvt_libgodot_bridge_force_link(void);

}
