#pragma once

#include <cstdint>
#include <memory>

#include "core/error/error_list.h"
#include "core/object/ref_counted.h"
#include "core/string/ustring.h"
#include "vtb/live2d/live2d_face_mapper.hpp"

class OpenVTDmaBufCapture;
class OpenVTFaceTrackingRuntime;

namespace vtb {

class Live2DRenderSession;

class EmbeddedLive2DApplication final {
public:
	EmbeddedLive2DApplication();
	~EmbeddedLive2DApplication();

	EmbeddedLive2DApplication(const EmbeddedLive2DApplication &) = delete;
	EmbeddedLive2DApplication &operator=(const EmbeddedLive2DApplication &) = delete;
	EmbeddedLive2DApplication(EmbeddedLive2DApplication &&) = delete;
	EmbeddedLive2DApplication &operator=(EmbeddedLive2DApplication &&) = delete;

	Error initialize_from_environment();
	Error prepare_frame(uint64_t p_monotonic_timestamp_ns);
	Error complete_frame();
	Error shutdown();

	bool should_stop() const noexcept;
	uint64_t completed_frames() const noexcept;
	const String &last_error() const noexcept;

private:
	Error fail(Error p_error, const String &p_message);
	Error configure_render_session();
	Error configure_output();
	Error configure_face_tracking();
	Error apply_latest_face_parameters(uint64_t p_monotonic_timestamp_ns);
	Error apply_fallback_parameters(uint64_t p_monotonic_timestamp_ns);

	std::unique_ptr<Live2DRenderSession> render_session_;
	Ref<OpenVTDmaBufCapture> capture_;
	Ref<OpenVTFaceTrackingRuntime> face_tracking_;
	String last_error_;
	Live2DParameters last_parameters_;
	uint64_t last_parameter_timestamp_ns_ = 0;
	uint64_t last_generation_ = 0;
	uint64_t completed_frames_ = 0;
	int output_width_ = 1920;
	int output_height_ = 1080;
	int output_fps_ = 30;
	int total_frames_ = 0;
	uint64_t maximum_parameter_age_ns_ = 150000000ULL;
	bool initialized_ = false;
	bool output_finished_ = false;
};

} // namespace vtb
