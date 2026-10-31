#pragma once

#include <cstdint>
#include <memory>

#include "core/error/error_list.h"
#include "core/object/ref_counted.h"
#include "core/string/ustring.h"
#include "vtb/live2d/live2d_parameters.hpp"

class OpenVTDmaBufCapture;

namespace vtb {

class Live2DRenderSession;

class EmbeddedLive2DApplicationConfig {
public:
	String live2d_model;
	String rtsp_url;
	int output_width = 0;
	int output_height = 0;
	int output_fps = 0;
	int total_frames = 0;
};

class EmbeddedLive2DApplication final {
public:
	EmbeddedLive2DApplication();
	~EmbeddedLive2DApplication();

	EmbeddedLive2DApplication(const EmbeddedLive2DApplication &) = delete;
	EmbeddedLive2DApplication &operator=(const EmbeddedLive2DApplication &) = delete;
	EmbeddedLive2DApplication(EmbeddedLive2DApplication &&) = delete;
	EmbeddedLive2DApplication &operator=(EmbeddedLive2DApplication &&) = delete;

	Error initialize(const EmbeddedLive2DApplicationConfig &p_config);
	Error submit_live2d_frame(
			const Live2DParameters &p_parameters,
			float p_view_zoom,
			float p_view_offset_x,
			float p_view_offset_y,
			uint64_t p_produced_timestamp_ns,
			uint64_t p_generation);
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
	Error apply_pending_parameters(uint64_t p_monotonic_timestamp_ns);
	Error apply_fallback_parameters(uint64_t p_monotonic_timestamp_ns);

	std::unique_ptr<Live2DRenderSession> render_session_;
	Ref<OpenVTDmaBufCapture> capture_;
	String last_error_;
	Live2DParameters last_parameters_;
	Live2DParameters pending_parameters_;
	uint64_t last_parameter_update_timestamp_ns_ = 0;
	uint64_t pending_produced_timestamp_ns_ = 0;
	uint64_t last_generation_ = 0;
	uint64_t pending_generation_ = 0;
	uint64_t completed_frames_ = 0;
	float pending_view_zoom_ = 1.0F;
	float pending_view_offset_x_ = 0.0F;
	float pending_view_offset_y_ = 0.0F;
	int output_width_ = 1920;
	int output_height_ = 1080;
	int output_fps_ = 30;
	int total_frames_ = 0;
	String live2d_model_;
	String rtsp_url_;
	uint64_t maximum_parameter_age_ns_ = 150000000ULL;
	bool has_pending_parameters_ = false;
	bool initialized_ = false;
	bool output_finished_ = false;
};

} // namespace vtb
