#include "embedded_live2d_application.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>

#include "core/string/print_string.h"
#include "live2d_render_session.h"
#include "openvt_dmabuf_capture.h"

namespace vtb {

namespace {

constexpr double kFallbackTauSeconds = 0.25;

String required_environment(const char *p_name, String &r_error) {
	const char *value = std::getenv(p_name);
	if (value == nullptr || value[0] == '\0') {
		r_error = String::utf8(p_name) + " is required";
		return String();
	}
	return String::utf8(value);
}

bool integer_environment(
		const char *p_name,
		int p_default,
		int p_minimum,
		int p_maximum,
		int &r_value,
		String &r_error) {
	const char *text = std::getenv(p_name);
	if (text == nullptr || text[0] == '\0') {
		r_value = p_default;
		return true;
	}
	errno = 0;
	char *end = nullptr;
	const long parsed = std::strtol(text, &end, 10);
	if (errno == ERANGE || end == text || end == nullptr || end[0] != '\0' ||
			parsed < p_minimum || parsed > p_maximum) {
		r_error = String::utf8(p_name) + " is outside the supported range";
		return false;
	}
	r_value = static_cast<int>(parsed);
	return true;
}

float approach(float p_current, float p_target, float p_alpha) {
	return p_current + p_alpha * (p_target - p_current);
}

Live2DParameters relaxed_parameters(const Live2DParameters &p_current, float p_alpha) {
	const Live2DParameters defaults;
	return Live2DParameters(
			approach(p_current.angle_x(), defaults.angle_x(), p_alpha),
			approach(p_current.angle_y(), defaults.angle_y(), p_alpha),
			approach(p_current.angle_z(), defaults.angle_z(), p_alpha),
			approach(p_current.body_angle_x(), defaults.body_angle_x(), p_alpha),
			approach(p_current.body_angle_y(), defaults.body_angle_y(), p_alpha),
			approach(p_current.body_angle_z(), defaults.body_angle_z(), p_alpha),
			approach(p_current.left_eye_open(), defaults.left_eye_open(), p_alpha),
			approach(p_current.right_eye_open(), defaults.right_eye_open(), p_alpha),
			approach(p_current.mouth_open(), defaults.mouth_open(), p_alpha),
			approach(p_current.mouth_form(), defaults.mouth_form(), p_alpha));
}

} // namespace

EmbeddedLive2DApplication::EmbeddedLive2DApplication() = default;

EmbeddedLive2DApplication::~EmbeddedLive2DApplication() {
	shutdown();
}

Error EmbeddedLive2DApplication::initialize_from_environment() {
	if (initialized_) {
		return fail(ERR_ALREADY_IN_USE, "Embedded Live2D application is already initialized");
	}
	String parse_error;
	if (!integer_environment("OPENVT_OUTPUT_WIDTH", 1920, 2, 8192, output_width_, parse_error) ||
			!integer_environment("OPENVT_OUTPUT_HEIGHT", 1080, 2, 8192, output_height_, parse_error) ||
			!integer_environment("OPENVT_OUTPUT_FPS", 30, 1, 240, output_fps_, parse_error) ||
			!integer_environment(
					"OPENVT_LIVE2D_TOTAL_FRAMES",
					0,
					0,
					std::numeric_limits<int>::max(),
					total_frames_,
					parse_error)) {
		return fail(ERR_INVALID_PARAMETER, parse_error);
	}
	if (output_width_ % 2 != 0 || output_height_ % 2 != 0) {
		return fail(ERR_INVALID_PARAMETER, "Output dimensions must be even for NV12 encoding");
	}

	Error error = configure_render_session();
	if (error == OK) {
		error = configure_output();
	}
	if (error != OK) {
		shutdown();
		return error;
	}
	initialized_ = true;
	print_line("VTB_CPP_RUNTIME_READY size=" + itos(output_width_) + "x" +
			itos(output_height_) + " fps=" + itos(output_fps_));
	return OK;
}

Error EmbeddedLive2DApplication::configure_render_session() {
	String error_message;
	const String model_path = required_environment("OPENVT_LIVE2D_MODEL", error_message);
	if (!error_message.is_empty()) {
		return fail(ERR_INVALID_PARAMETER, error_message);
	}
	render_session_ = std::make_unique<Live2DRenderSession>();
	Live2DRenderSessionConfig config;
	config.model_path = model_path;
	config.output_size = Vector2i(output_width_, output_height_);
	Error error = render_session_->initialize(config);
	if (error == OK) {
		error = render_session_->load_model();
	}
	if (error == OK) {
		error = render_session_->start();
	}
	if (error != OK) {
		return fail(error, "Live2D render session failed: " + render_session_->last_error());
	}
	return OK;
}

Error EmbeddedLive2DApplication::configure_output() {
	String error_message;
	const String rtsp_url = required_environment("OPENVT_RTSP_URL", error_message);
	if (!error_message.is_empty()) {
		return fail(ERR_INVALID_PARAMETER, error_message);
	}
	capture_.instantiate();
	const Error error = capture_->configure(
			rtsp_url, output_width_, output_height_, output_fps_, total_frames_);
	if (error != OK) {
		return fail(error, "DMA-BUF output configuration failed: " + capture_->get_last_error());
	}
	return OK;
}

Error EmbeddedLive2DApplication::prepare_frame(uint64_t p_monotonic_timestamp_ns) {
	if (!initialized_ || render_session_ == nullptr) {
		return fail(ERR_UNCONFIGURED, "Embedded Live2D application is not initialized");
	}
	if (output_finished_) {
		return OK;
	}
	return apply_pending_parameters(p_monotonic_timestamp_ns);
}

Error EmbeddedLive2DApplication::submit_live2d_frame(
		const Live2DParameters &p_parameters,
		float p_view_zoom,
		float p_view_offset_x,
		float p_view_offset_y,
		uint64_t p_produced_timestamp_ns,
		uint64_t p_generation) {
	if (!initialized_ || render_session_ == nullptr) {
		return fail(ERR_UNCONFIGURED, "Embedded Live2D application is not initialized");
	}
	if (!std::isfinite(p_view_zoom) || !std::isfinite(p_view_offset_x) ||
			!std::isfinite(p_view_offset_y) || p_view_zoom <= 0.0F ||
			p_produced_timestamp_ns == 0 || p_generation == 0) {
		return fail(ERR_INVALID_PARAMETER, "Live2D frame input is invalid");
	}
	pending_parameters_ = p_parameters;
	pending_view_zoom_ = p_view_zoom;
	pending_view_offset_x_ = p_view_offset_x;
	pending_view_offset_y_ = p_view_offset_y;
	pending_produced_timestamp_ns_ = p_produced_timestamp_ns;
	pending_generation_ = p_generation;
	has_pending_parameters_ = true;
	return OK;
}

Error EmbeddedLive2DApplication::apply_pending_parameters(uint64_t p_monotonic_timestamp_ns) {
	if (!has_pending_parameters_) {
		return apply_fallback_parameters(p_monotonic_timestamp_ns);
	}
	has_pending_parameters_ = false;
	uint64_t age_ns = 0;
	if (p_monotonic_timestamp_ns >= pending_produced_timestamp_ns_) {
		age_ns = p_monotonic_timestamp_ns - pending_produced_timestamp_ns_;
	}
	if (age_ns > maximum_parameter_age_ns_) {
		return apply_fallback_parameters(p_monotonic_timestamp_ns);
	}
	last_parameters_ = pending_parameters_;
	Error error = render_session_->submit_parameters(last_parameters_);
	if (error != OK) {
		return fail(error, "Live2D parameter submission failed: " + render_session_->last_error());
	}
	Live2DRenderViewState view_state(
			pending_view_zoom_, pending_view_offset_x_, pending_view_offset_y_);
	error = render_session_->set_view_state(view_state);
	if (error != OK) {
		return fail(error, "Live2D view update failed: " + render_session_->last_error());
	}
	last_parameter_update_timestamp_ns_ = p_monotonic_timestamp_ns;
	last_generation_ = pending_generation_;
	return OK;
}

Error EmbeddedLive2DApplication::apply_fallback_parameters(uint64_t p_monotonic_timestamp_ns) {
	float delta_seconds = 1.0F / static_cast<float>(output_fps_);
	if (last_parameter_update_timestamp_ns_ > 0 &&
			p_monotonic_timestamp_ns > last_parameter_update_timestamp_ns_) {
		delta_seconds = static_cast<float>(
				static_cast<double>(p_monotonic_timestamp_ns - last_parameter_update_timestamp_ns_) /
				1000000000.0);
	}
	const float alpha = static_cast<float>(1.0 - std::exp(-delta_seconds / kFallbackTauSeconds));
	last_parameters_ = relaxed_parameters(last_parameters_, alpha);
	last_parameter_update_timestamp_ns_ = p_monotonic_timestamp_ns;
	const Error error = render_session_->submit_parameters(last_parameters_);
	if (error != OK) {
		return fail(error, "Live2D fallback submission failed: " + render_session_->last_error());
	}
	return OK;
}

Error EmbeddedLive2DApplication::complete_frame() {
	if (!initialized_ || capture_.is_null() || render_session_ == nullptr) {
		return fail(ERR_UNCONFIGURED, "Embedded Live2D application is not initialized");
	}
	if (output_finished_) {
		return OK;
	}
	const uint64_t next_frame = completed_frames_ + 1;
	bool end_of_stream = false;
	if (total_frames_ > 0 && next_frame >= static_cast<uint64_t>(total_frames_)) {
		end_of_stream = true;
	}
	const Error error = capture_->capture_frame(
			render_session_->viewport_rid(), static_cast<int>(completed_frames_), end_of_stream);
	if (error != OK) {
		return fail(error, "DMA-BUF frame capture failed: " + capture_->get_last_error());
	}
	completed_frames_ = next_frame;
	if (completed_frames_ == 1 || completed_frames_ % static_cast<uint64_t>(output_fps_) == 0) {
		print_line("VTB_CPP_FRAME_CAPTURED frame=" + itos(static_cast<int64_t>(completed_frames_)) +
				" face_generation=" + itos(static_cast<int64_t>(last_generation_)));
	}
	if (end_of_stream) {
		const Error finish_error = capture_->finish();
		if (finish_error != OK) {
			return fail(finish_error, "DMA-BUF output finish failed: " + capture_->get_last_error());
		}
		output_finished_ = true;
	}
	return OK;
}

Error EmbeddedLive2DApplication::shutdown() {
	Error result = OK;
	if (capture_.is_valid()) {
		const Error capture_error = capture_->finish();
		if (capture_error != OK && result == OK) {
			result = capture_error;
			last_error_ = "DMA-BUF output shutdown failed: " + capture_->get_last_error();
		}
		capture_.unref();
	}
	if (render_session_ != nullptr) {
		render_session_->shutdown();
		render_session_.reset();
	}
	initialized_ = false;
	output_finished_ = true;
	return result;
}

bool EmbeddedLive2DApplication::should_stop() const noexcept {
	return output_finished_;
}

uint64_t EmbeddedLive2DApplication::completed_frames() const noexcept {
	return completed_frames_;
}

const String &EmbeddedLive2DApplication::last_error() const noexcept {
	return last_error_;
}

Error EmbeddedLive2DApplication::fail(Error p_error, const String &p_message) {
	last_error_ = p_message;
	ERR_PRINT("VTB_CPP_RUNTIME_ERROR " + p_message);
	return p_error;
}

} // namespace vtb
