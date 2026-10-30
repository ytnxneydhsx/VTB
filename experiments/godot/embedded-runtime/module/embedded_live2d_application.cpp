#include "embedded_live2d_application.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>

#include "core/string/print_string.h"
#include "core/variant/dictionary.h"
#include "live2d_render_session.h"
#include "openvt_dmabuf_capture.h"
#include "openvt_face_tracking_runtime.h"

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

String optional_environment(const char *p_name, const char *p_default) {
	const char *value = std::getenv(p_name);
	if (value == nullptr || value[0] == '\0') {
		return String::utf8(p_default);
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

bool floating_environment(
		const char *p_name,
		double p_default,
		double &r_value,
		String &r_error) {
	const char *text = std::getenv(p_name);
	if (text == nullptr || text[0] == '\0') {
		r_value = p_default;
		return true;
	}
	errno = 0;
	char *end = nullptr;
	const double parsed = std::strtod(text, &end);
	if (errno == ERANGE || end == text || end == nullptr || end[0] != '\0' ||
			!std::isfinite(parsed)) {
		r_error = String::utf8(p_name) + " must be a finite number";
		return false;
	}
	r_value = parsed;
	return true;
}

float dictionary_float(const Dictionary &p_dictionary, const char *p_key, float p_default) {
	const String key = String::utf8(p_key);
	if (!p_dictionary.has(key)) {
		return p_default;
	}
	return static_cast<float>(p_dictionary[key]);
}

Live2DParameters parameters_from_dictionary(const Dictionary &p_parameters) {
	return Live2DParameters(
			dictionary_float(p_parameters, "ParamAngleX", 0.0F),
			dictionary_float(p_parameters, "ParamAngleY", 0.0F),
			dictionary_float(p_parameters, "ParamAngleZ", 0.0F),
			dictionary_float(p_parameters, "ParamBodyAngleX", 0.0F),
			dictionary_float(p_parameters, "ParamBodyAngleY", 0.0F),
			dictionary_float(p_parameters, "ParamBodyAngleZ", 0.0F),
			dictionary_float(p_parameters, "ParamEyeLOpen", 1.0F),
			dictionary_float(p_parameters, "ParamEyeROpen", 1.0F),
			dictionary_float(p_parameters, "ParamMouthOpenY", 0.0F),
			dictionary_float(p_parameters, "ParamMouthForm", 0.0F));
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
	if (error == OK) {
		error = configure_face_tracking();
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

Error EmbeddedLive2DApplication::configure_face_tracking() {
	String error_message;
	const String source = required_environment("OPENVT_FACE_SOURCE", error_message);
	const String detector = required_environment("OPENVT_FACE_DETECTOR_MODEL", error_message);
	const String landmark = required_environment("OPENVT_FACE_LANDMARK_MODEL", error_message);
	if (!error_message.is_empty()) {
		return fail(ERR_INVALID_PARAMETER, error_message);
	}
	int width = 1280;
	int height = 720;
	int fps = 60;
	int buffers = 4;
	int calibration_frames = 30;
	int control_port = 11575;
	double smoothing_ms = 50.0;
	double angle_x_sign = -1.0;
	double angle_y_sign = -1.0;
	double angle_z_sign = -1.0;
	if (!integer_environment("OPENVT_FACE_WIDTH", 1280, 1, 8192, width, error_message) ||
			!integer_environment("OPENVT_FACE_HEIGHT", 720, 1, 8192, height, error_message) ||
			!integer_environment("OPENVT_FACE_FPS", 60, 1, 240, fps, error_message) ||
			!integer_environment("OPENVT_FACE_BUFFERS", 4, 2, 64, buffers, error_message) ||
			!integer_environment(
					"OPENVT_FACE_CALIBRATION_FRAMES", 30, 1, 10000, calibration_frames, error_message) ||
			!integer_environment(
					"OPENVT_LIVE2D_CONTROL_PORT", 11575, 1, 65535, control_port, error_message) ||
			!floating_environment("OPENVT_FACE_SMOOTHING_MS", 50.0, smoothing_ms, error_message) ||
			!floating_environment("OPENVT_FACE_ANGLE_X_SIGN", -1.0, angle_x_sign, error_message) ||
			!floating_environment("OPENVT_FACE_ANGLE_Y_SIGN", -1.0, angle_y_sign, error_message) ||
			!floating_environment("OPENVT_FACE_ANGLE_Z_SIGN", -1.0, angle_z_sign, error_message)) {
		return fail(ERR_INVALID_PARAMETER, error_message);
	}

	face_tracking_.instantiate();
	Error error = face_tracking_->configure(
			source,
			width,
			height,
			fps,
			buffers,
			detector,
			landmark,
			calibration_frames,
			smoothing_ms,
			angle_x_sign,
			angle_y_sign,
			angle_z_sign,
			optional_environment("OPENVT_LIVE2D_CONTROL_ADDRESS", "192.168.77.2"),
			control_port,
			optional_environment("OPENVT_LIVE2D_CONTROL_ORIGIN", "http://192.168.77.1:8900"));
	if (error == OK) {
		error = face_tracking_->start();
	}
	if (error != OK) {
		return fail(error, "Face tracking start failed: " + face_tracking_->get_last_error());
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
	return apply_latest_face_parameters(p_monotonic_timestamp_ns);
}

Error EmbeddedLive2DApplication::apply_latest_face_parameters(uint64_t p_monotonic_timestamp_ns) {
	Dictionary packet = face_tracking_->take_latest();
	if (packet.is_empty()) {
		return apply_fallback_parameters(p_monotonic_timestamp_ns);
	}
	const int64_t age_usec = packet.get("age_usec", -1);
	if (age_usec < 0 || static_cast<uint64_t>(age_usec) * 1000ULL > maximum_parameter_age_ns_) {
		return apply_fallback_parameters(p_monotonic_timestamp_ns);
	}
	Dictionary parameters = packet.get("parameters", Dictionary());
	last_parameters_ = parameters_from_dictionary(parameters);
	Error error = render_session_->submit_parameters(last_parameters_);
	if (error != OK) {
		return fail(error, "Live2D parameter submission failed: " + render_session_->last_error());
	}
	Dictionary view = packet.get("view", Dictionary());
	if (!view.is_empty()) {
		Live2DRenderViewState view_state(
				dictionary_float(view, "zoom", 1.0F),
				dictionary_float(view, "offset_x", 0.0F),
				dictionary_float(view, "offset_y", 0.0F));
		error = render_session_->set_view_state(view_state);
		if (error != OK) {
			return fail(error, "Live2D view update failed: " + render_session_->last_error());
		}
	}
	last_parameter_timestamp_ns_ = p_monotonic_timestamp_ns;
	last_generation_ = static_cast<uint64_t>(static_cast<int64_t>(packet.get("generation", 0)));
	return OK;
}

Error EmbeddedLive2DApplication::apply_fallback_parameters(uint64_t p_monotonic_timestamp_ns) {
	float delta_seconds = 1.0F / static_cast<float>(output_fps_);
	if (last_parameter_timestamp_ns_ > 0 && p_monotonic_timestamp_ns > last_parameter_timestamp_ns_) {
		delta_seconds = static_cast<float>(
				static_cast<double>(p_monotonic_timestamp_ns - last_parameter_timestamp_ns_) / 1000000000.0);
	}
	const float alpha = static_cast<float>(1.0 - std::exp(-delta_seconds / kFallbackTauSeconds));
	last_parameters_ = relaxed_parameters(last_parameters_, alpha);
	last_parameter_timestamp_ns_ = p_monotonic_timestamp_ns;
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
	if (face_tracking_.is_valid()) {
		const Error face_error = face_tracking_->stop();
		if (face_error != OK) {
			result = face_error;
			last_error_ = "Face tracking shutdown failed: " + face_tracking_->get_last_error();
		}
		face_tracking_.unref();
	}
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
