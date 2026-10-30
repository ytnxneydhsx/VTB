#include "openvt_face_tracking_runtime.h"

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <time.h>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "core/object/class_db.h"

#include "vtb/face/cpu_image_transformer.hpp"
#include "vtb/face/openseeface_geometry_estimator.hpp"
#include "vtb/face/rknn_face_models.hpp"
#include "vtb/live2d/live2d_face_mapper.hpp"
#include "vtb/pipeline/pipeline_runtime.hpp"
#include "vtb/video/opencv_video_source.hpp"

namespace {

constexpr double kViewZoomMinimum = 0.5;
constexpr double kViewZoomMaximum = 6.0;
constexpr double kViewOffsetMinimum = -2.0;
constexpr double kViewOffsetMaximum = 2.0;

class Live2DViewState final {
public:
	Live2DViewState() = default;
	Live2DViewState(double p_zoom, double p_offset_x, double p_offset_y) :
			zoom(p_zoom), offset_x(p_offset_x), offset_y(p_offset_y) {}
	~Live2DViewState() = default;

	Live2DViewState(const Live2DViewState &) = default;
	Live2DViewState &operator=(const Live2DViewState &) = default;
	Live2DViewState(Live2DViewState &&) noexcept = default;
	Live2DViewState &operator=(Live2DViewState &&) noexcept = default;

	double zoom = 1.0;
	double offset_x = 0.0;
	double offset_y = 0.0;
};

uint64_t monotonic_now_ns() {
	timespec timestamp{};
	if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0) {
		throw std::runtime_error("clock_gettime(CLOCK_MONOTONIC) failed");
	}
	return static_cast<uint64_t>(timestamp.tv_sec) * 1000000000ULL +
			static_cast<uint64_t>(timestamp.tv_nsec);
}

class FaceParameterMailbox final {
public:
	explicit FaceParameterMailbox(vtb::Live2DMappingConfig config) :
			mapper(config), calibration_required(config.calibration_frames) {}
	~FaceParameterMailbox() = default;

	FaceParameterMailbox(const FaceParameterMailbox &) = delete;
	FaceParameterMailbox &operator=(const FaceParameterMailbox &) = delete;
	FaceParameterMailbox(FaceParameterMailbox &&) = delete;
	FaceParameterMailbox &operator=(FaceParameterMailbox &&) = delete;

	void publish(const std::optional<vtb::FaceTrackingResult> &result) {
		const uint64_t requested_reset = reset_generation.load(std::memory_order_acquire);
		if (requested_reset != applied_reset_generation) {
			mapper.reset_calibration();
			applied_reset_generation = requested_reset;
			reset_pending.store(false, std::memory_order_release);
		}
		const vtb::FaceTrackingResult *face = nullptr;
		if (result.has_value()) {
			face = &result.value();
		}
		vtb::Live2DMappingResult mapped = mapper.update(face, monotonic_now_ns());
		std::lock_guard<std::mutex> lock(mutex);
		latest = std::move(mapped);
		++generation;
	}

	std::optional<vtb::Live2DMappingResult> read(uint64_t &r_generation) const {
		std::lock_guard<std::mutex> lock(mutex);
		r_generation = generation;
		return latest;
	}

	void request_calibration_reset() noexcept {
		reset_pending.store(true, std::memory_order_release);
		reset_generation.fetch_add(1, std::memory_order_release);
	}

	bool set_view(const Live2DViewState &p_view) noexcept {
		if (!std::isfinite(p_view.zoom) || !std::isfinite(p_view.offset_x) ||
				!std::isfinite(p_view.offset_y) ||
				p_view.zoom < kViewZoomMinimum || p_view.zoom > kViewZoomMaximum ||
				p_view.offset_x < kViewOffsetMinimum || p_view.offset_x > kViewOffsetMaximum ||
				p_view.offset_y < kViewOffsetMinimum || p_view.offset_y > kViewOffsetMaximum) {
			return false;
		}
		std::lock_guard<std::mutex> lock(mutex);
		view = p_view;
		return true;
	}

	Live2DViewState read_view() const {
		std::lock_guard<std::mutex> lock(mutex);
		return view;
	}

	std::string health_json() const {
		std::lock_guard<std::mutex> lock(mutex);
		bool calibrated = false;
		bool tracking_valid = false;
		std::size_t calibration_progress = 0;
		float confidence = 0.0F;
		if (latest.has_value()) {
			calibrated = latest->calibrated();
			tracking_valid = latest->tracking_valid();
			calibration_progress = latest->calibration_progress();
			confidence = latest->confidence();
		}
		if (reset_pending.load(std::memory_order_acquire)) {
			calibrated = false;
			calibration_progress = 0;
		}
		std::ostringstream body;
		body << "{\"state\":\"running\",\"calibrated\":";
		if (calibrated) {
			body << "true";
		} else {
			body << "false";
		}
		body << ",\"tracking_valid\":";
		if (tracking_valid) {
			body << "true";
		} else {
			body << "false";
		}
		body << ",\"progress\":" << calibration_progress;
		body << ",\"required\":" << calibration_required;
		body << ",\"confidence\":" << confidence;
		body << ",\"view\":{\"zoom\":" << view.zoom;
		body << ",\"offset_x\":" << view.offset_x;
		body << ",\"offset_y\":" << view.offset_y << "}}";
		return body.str();
	}

private:
	mutable std::mutex mutex;
	vtb::Live2DFaceMapper mapper;
	std::size_t calibration_required = 0;
	std::optional<vtb::Live2DMappingResult> latest;
	std::atomic<uint64_t> reset_generation{ 0 };
	std::atomic<bool> reset_pending{ false };
	uint64_t applied_reset_generation = 0;
	uint64_t generation = 0;
	Live2DViewState view;
};

class Live2DControlServer final {
public:
	Live2DControlServer(
			std::string p_bind_address,
			int p_port,
			std::string p_allowed_origin,
			std::shared_ptr<FaceParameterMailbox> p_mailbox) :
			bind_address(std::move(p_bind_address)),
			port(p_port),
			allowed_origin(std::move(p_allowed_origin)),
			mailbox(std::move(p_mailbox)) {
		if (bind_address.empty() || port < 1 || port > 65535 || mailbox == nullptr) {
			throw std::invalid_argument("Live2D control server configuration is invalid");
		}
	}

	~Live2DControlServer() {
		stop();
	}

	Live2DControlServer(const Live2DControlServer &) = delete;
	Live2DControlServer &operator=(const Live2DControlServer &) = delete;
	Live2DControlServer(Live2DControlServer &&) = delete;
	Live2DControlServer &operator=(Live2DControlServer &&) = delete;

	void start() {
		if (worker.joinable()) {
			return;
		}
		int created_socket = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (created_socket < 0) {
			throw std::runtime_error("Unable to create Live2D control socket");
		}
		int reuse_address = 1;
		setsockopt(created_socket, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address));
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_port = htons(static_cast<uint16_t>(port));
		if (inet_pton(AF_INET, bind_address.c_str(), &address.sin_addr) != 1) {
			close(created_socket);
			throw std::invalid_argument("Live2D control bind address must be an IPv4 address");
		}
		if (bind(created_socket, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 ||
				listen(created_socket, 8) != 0) {
			const int saved_errno = errno;
			close(created_socket);
			throw std::runtime_error(
					"Unable to bind Live2D control socket: " + std::string(std::strerror(saved_errno)));
		}
		listen_socket.store(created_socket, std::memory_order_release);
		stop_requested.store(false, std::memory_order_release);
		worker = std::thread([this]() { run(); });
	}

	void stop() noexcept {
		stop_requested.store(true, std::memory_order_release);
		const int socket_to_close = listen_socket.exchange(-1, std::memory_order_acq_rel);
		if (socket_to_close >= 0) {
			shutdown(socket_to_close, SHUT_RDWR);
			close(socket_to_close);
		}
		if (worker.joinable()) {
			worker.join();
		}
	}

private:
	static std::string header_value(const std::string &p_request, const std::string &p_name) {
		const std::string needle = "\r\n" + p_name + ":";
		const std::size_t position = p_request.find(needle);
		if (position == std::string::npos) {
			return std::string();
		}
		std::size_t value_start = position + needle.size();
		while (value_start < p_request.size() && p_request[value_start] == ' ') {
			++value_start;
		}
		const std::size_t value_end = p_request.find("\r\n", value_start);
		if (value_end == std::string::npos) {
			return std::string();
		}
		return p_request.substr(value_start, value_end - value_start);
	}

	static bool parse_json_number(
			const std::string &p_body,
			const std::string &p_name,
			double &r_value) noexcept {
		const std::string key = "\"" + p_name + "\"";
		const std::size_t key_position = p_body.find(key);
		if (key_position == std::string::npos) {
			return false;
		}
		const std::size_t colon = p_body.find(':', key_position + key.size());
		if (colon == std::string::npos) {
			return false;
		}
		const char *number_start = p_body.c_str() + colon + 1U;
		char *number_end = nullptr;
		errno = 0;
		const double parsed = std::strtod(number_start, &number_end);
		if (number_start == number_end || errno == ERANGE || !std::isfinite(parsed)) {
			return false;
		}
		r_value = parsed;
		return true;
	}

	bool origin_is_allowed(const std::string &p_request) const {
		if (allowed_origin.empty()) {
			return true;
		}
		const std::string origin = header_value(p_request, "Origin");
		if (origin.empty()) {
			return true;
		}
		return origin == allowed_origin;
	}

	std::string cors_headers(const std::string &p_request) const {
		if (allowed_origin.empty()) {
			return std::string();
		}
		if (header_value(p_request, "Origin") != allowed_origin) {
			return std::string();
		}
		return "Access-Control-Allow-Origin: " + allowed_origin + "\r\nVary: Origin\r\n";
	}

	void send_response(
			int p_client,
			const std::string &p_request,
			int p_status,
			const std::string &p_reason,
			const std::string &p_body,
			const std::string &p_extra_headers = std::string()) const noexcept {
		std::ostringstream response;
		response << "HTTP/1.1 " << p_status << " " << p_reason << "\r\n";
		response << cors_headers(p_request);
		response << p_extra_headers;
		response << "Cache-Control: no-store\r\n";
		response << "Content-Type: application/json; charset=utf-8\r\n";
		response << "Content-Length: " << p_body.size() << "\r\n";
		response << "Connection: close\r\n\r\n" << p_body;
		const std::string encoded = response.str();
		std::size_t sent = 0;
		while (sent < encoded.size()) {
			const ssize_t result = send(
					p_client, encoded.data() + sent, encoded.size() - sent, MSG_NOSIGNAL);
			if (result <= 0) {
				return;
			}
			sent += static_cast<std::size_t>(result);
		}
	}

	std::string read_request(int p_client) const {
		timeval timeout{};
		timeout.tv_sec = 1;
		setsockopt(p_client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
		std::string request;
		request.reserve(2048);
		std::size_t expected_size = 0;
		while (request.size() < 8192U) {
			char buffer[1024];
			const ssize_t received = recv(p_client, buffer, sizeof(buffer), 0);
			if (received <= 0) {
				break;
			}
			request.append(buffer, static_cast<std::size_t>(received));
			const std::size_t header_end = request.find("\r\n\r\n");
			if (header_end == std::string::npos) {
				continue;
			}
			if (expected_size == 0) {
				const std::string content_length = header_value(request, "Content-Length");
				std::size_t body_size = 0;
				if (!content_length.empty()) {
					body_size = static_cast<std::size_t>(std::stoul(content_length));
				}
				expected_size = header_end + 4U + body_size;
			}
			if (request.size() >= expected_size) {
				break;
			}
		}
		return request;
	}

	void handle_client(int p_client) noexcept {
		try {
			const std::string request = read_request(p_client);
			const std::size_t first_line_end = request.find("\r\n");
			const std::size_t header_end = request.find("\r\n\r\n");
			if (first_line_end == std::string::npos || header_end == std::string::npos) {
				send_response(p_client, request, 400, "Bad Request", "{\"error\":\"invalid request\"}");
				return;
			}
			const std::string first_line = request.substr(0, first_line_end);
			if (first_line == "GET /health HTTP/1.1") {
				send_response(p_client, request, 200, "OK", mailbox->health_json());
				return;
			}
			if (first_line == "OPTIONS /recalibrate HTTP/1.1" ||
					first_line == "OPTIONS /view HTTP/1.1") {
				if (!origin_is_allowed(request)) {
					send_response(p_client, request, 403, "Forbidden", "{\"error\":\"origin is not allowed\"}");
					return;
				}
				send_response(
						p_client,
						request,
						204,
						"No Content",
						std::string(),
						"Access-Control-Allow-Methods: POST, OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type\r\n");
				return;
			}
			if (!origin_is_allowed(request)) {
				send_response(p_client, request, 403, "Forbidden", "{\"error\":\"origin is not allowed\"}");
				return;
			}
			if (first_line == "POST /recalibrate HTTP/1.1") {
				mailbox->request_calibration_reset();
				send_response(p_client, request, 202, "Accepted", mailbox->health_json());
				return;
			}
			if (first_line == "POST /view HTTP/1.1") {
				const std::string body = request.substr(header_end + 4U);
				Live2DViewState view;
				if (!parse_json_number(body, "zoom", view.zoom) ||
						!parse_json_number(body, "offset_x", view.offset_x) ||
						!parse_json_number(body, "offset_y", view.offset_y) ||
						!mailbox->set_view(view)) {
					send_response(p_client, request, 400, "Bad Request", "{\"error\":\"invalid view\"}");
					return;
				}
				send_response(p_client, request, 200, "OK", mailbox->health_json());
				return;
			}
			send_response(p_client, request, 404, "Not Found", "{\"error\":\"not found\"}");
		} catch (...) {
			send_response(p_client, std::string(), 400, "Bad Request", "{\"error\":\"invalid request\"}");
		}
	}

	void run() noexcept {
		while (!stop_requested.load(std::memory_order_acquire)) {
			const int current_socket = listen_socket.load(std::memory_order_acquire);
			if (current_socket < 0) {
				break;
			}
			pollfd descriptor{};
			descriptor.fd = current_socket;
			descriptor.events = POLLIN;
			const int poll_result = poll(&descriptor, 1, 200);
			if (poll_result <= 0 || (descriptor.revents & POLLIN) == 0) {
				continue;
			}
			const int client = accept4(current_socket, nullptr, nullptr, SOCK_CLOEXEC);
			if (client < 0) {
				continue;
			}
			handle_client(client);
			shutdown(client, SHUT_RDWR);
			close(client);
		}
	}

	std::string bind_address;
	int port = 0;
	std::string allowed_origin;
	std::shared_ptr<FaceParameterMailbox> mailbox;
	std::atomic<int> listen_socket{ -1 };
	std::atomic<bool> stop_requested{ false };
	std::thread worker;
};

Dictionary parameters_to_dictionary(const vtb::Live2DParameters &p_parameters) {
	Dictionary parameters;
	parameters["ParamAngleX"] = p_parameters.angle_x();
	parameters["ParamAngleY"] = p_parameters.angle_y();
	parameters["ParamAngleZ"] = p_parameters.angle_z();
	parameters["ParamBodyAngleX"] = p_parameters.body_angle_x();
	parameters["ParamBodyAngleY"] = p_parameters.body_angle_y();
	parameters["ParamBodyAngleZ"] = p_parameters.body_angle_z();
	parameters["ParamEyeLOpen"] = p_parameters.left_eye_open();
	parameters["ParamEyeROpen"] = p_parameters.right_eye_open();
	parameters["ParamMouthOpenY"] = p_parameters.mouth_open();
	parameters["ParamMouthForm"] = p_parameters.mouth_form();
	return parameters;
}

} // namespace

class OpenVTFaceTrackingRuntimeState final {
public:
	OpenVTFaceTrackingRuntimeState(
			std::unique_ptr<vtb::PipelineRuntime> p_runtime,
			std::shared_ptr<FaceParameterMailbox> p_mailbox,
			std::unique_ptr<Live2DControlServer> p_control_server) :
			runtime(std::move(p_runtime)),
			mailbox(std::move(p_mailbox)),
			control_server(std::move(p_control_server)) {}
	~OpenVTFaceTrackingRuntimeState() = default;

	OpenVTFaceTrackingRuntimeState(const OpenVTFaceTrackingRuntimeState &) = delete;
	OpenVTFaceTrackingRuntimeState &operator=(const OpenVTFaceTrackingRuntimeState &) = delete;
	OpenVTFaceTrackingRuntimeState(OpenVTFaceTrackingRuntimeState &&) = delete;
	OpenVTFaceTrackingRuntimeState &operator=(OpenVTFaceTrackingRuntimeState &&) = delete;

	std::unique_ptr<vtb::PipelineRuntime> runtime;
	std::shared_ptr<FaceParameterMailbox> mailbox;
	std::unique_ptr<Live2DControlServer> control_server;
};

OpenVTFaceTrackingRuntime::OpenVTFaceTrackingRuntime() = default;

OpenVTFaceTrackingRuntime::~OpenVTFaceTrackingRuntime() {
	stop();
}

Error OpenVTFaceTrackingRuntime::configure(
		const String &p_source,
		int p_width,
		int p_height,
		int p_fps,
		int p_buffer_count,
		const String &p_detector_model,
		const String &p_landmark_model,
		int p_calibration_frames,
		double p_smoothing_tau_ms,
		double p_angle_x_sign,
		double p_angle_y_sign,
		double p_angle_z_sign,
		const String &p_control_bind_address,
		int p_control_port,
		const String &p_control_allowed_origin) {
	if (configured || running) {
		set_error("Face tracking runtime is already configured");
		return ERR_ALREADY_IN_USE;
	}
	if (p_width <= 0 || p_height <= 0 || p_fps <= 0 || p_buffer_count < 2 ||
			p_calibration_frames <= 0 || p_control_port < 1 || p_control_port > 65535 ||
			p_control_bind_address.is_empty()) {
		set_error("Face tracking configuration contains an invalid numeric value");
		return ERR_INVALID_PARAMETER;
	}
	try {
		vtb::OpenCvVideoSourceConfig source_config;
		source_config.source = p_source.utf8().get_data();
		source_config.width = static_cast<uint32_t>(p_width);
		source_config.height = static_cast<uint32_t>(p_height);
		source_config.fps = static_cast<uint32_t>(p_fps);
		source_config.buffer_count = static_cast<uint32_t>(p_buffer_count);

		vtb::Live2DMappingConfig mapping_config;
		mapping_config.calibration_frames = static_cast<std::size_t>(p_calibration_frames);
		mapping_config.smoothing_tau_ms = static_cast<float>(p_smoothing_tau_ms);
		mapping_config.angle_x_sign = static_cast<float>(p_angle_x_sign);
		mapping_config.angle_y_sign = static_cast<float>(p_angle_y_sign);
		mapping_config.angle_z_sign = static_cast<float>(p_angle_z_sign);

		auto mailbox = std::make_shared<FaceParameterMailbox>(mapping_config);
		auto image_transformer = std::make_unique<vtb::CpuImageTransformer>();
		auto detector = std::make_unique<vtb::RknnFaceDetector>(
				p_detector_model.utf8().get_data(), vtb::RknnCore::core0);
		auto landmark = std::make_unique<vtb::RknnLandmarkEstimator>(
				p_landmark_model.utf8().get_data(), vtb::RknnCore::core1);
		auto geometry = std::make_unique<vtb::OpenSeeFaceGeometryEstimator>();
		vtb::FacePipelineConfig face_config;
		face_config.redetect_interval_frames = 0;
		face_config.minimum_tracking_confidence = 0.6F;
		auto face_pipeline = std::make_unique<vtb::FacePipeline>(
				face_config,
				std::move(image_transformer),
				std::move(detector),
				std::move(landmark),
				std::move(geometry));
		auto source = std::make_unique<vtb::OpenCvVideoSource>(std::move(source_config));
		auto runtime = std::make_unique<vtb::PipelineRuntime>(
				std::move(source),
				std::move(face_pipeline),
				[mailbox](const std::optional<vtb::FaceTrackingResult> &p_result) {
					mailbox->publish(p_result);
				});
		auto control_server = std::make_unique<Live2DControlServer>(
				p_control_bind_address.utf8().get_data(),
				p_control_port,
				p_control_allowed_origin.utf8().get_data(),
				mailbox);
		state = std::make_unique<OpenVTFaceTrackingRuntimeState>(
				std::move(runtime), std::move(mailbox), std::move(control_server));
		configured = true;
		last_error = String();
		return OK;
	} catch (const std::exception &error) {
		set_error(String::utf8(error.what()));
		state.reset();
		return ERR_CANT_CREATE;
	} catch (...) {
		set_error("Face tracking configure failed with an unknown exception");
		state.reset();
		return ERR_CANT_CREATE;
	}
}

Error OpenVTFaceTrackingRuntime::start() {
	if (!configured || state == nullptr || state->runtime == nullptr) {
		set_error("Face tracking runtime is not configured");
		return ERR_UNCONFIGURED;
	}
	if (running) {
		return OK;
	}
	try {
		state->runtime->start();
		if (state->control_server != nullptr) {
			state->control_server->start();
		}
		running = true;
		return OK;
	} catch (const std::exception &error) {
		try {
			state->runtime->stop();
		} catch (...) {
		}
		set_error(String::utf8(error.what()));
		return FAILED;
	} catch (...) {
		try {
			state->runtime->stop();
		} catch (...) {
		}
		set_error("Face tracking start failed with an unknown exception");
		return FAILED;
	}
}

Dictionary OpenVTFaceTrackingRuntime::take_latest() {
	Dictionary result;
	if (!configured || state == nullptr || state->mailbox == nullptr) {
		return result;
	}
	try {
		if (state->runtime != nullptr) {
			state->runtime->rethrow_if_failed();
		}
		uint64_t generation = 0;
		std::optional<vtb::Live2DMappingResult> latest = state->mailbox->read(generation);
		if (!latest.has_value()) {
			return result;
		}
		result["generation"] = static_cast<int64_t>(generation);
		result["frame_id"] = static_cast<int64_t>(latest->frame_id());
		result["result_ready_ns"] = static_cast<int64_t>(latest->produced_timestamp_ns());
		const uint64_t current_ns = monotonic_now_ns();
		uint64_t age_ns = 0;
		if (current_ns >= latest->produced_timestamp_ns()) {
			age_ns = current_ns - latest->produced_timestamp_ns();
		}
		result["age_usec"] = static_cast<int64_t>(age_ns / 1000ULL);
		result["tracking_valid"] = latest->tracking_valid();
		result["calibrated"] = latest->calibrated();
		result["calibration_progress"] = static_cast<int64_t>(latest->calibration_progress());
		result["confidence"] = latest->confidence();
		result["parameters"] = parameters_to_dictionary(latest->parameters());
		const Live2DViewState view = state->mailbox->read_view();
		Dictionary view_dictionary;
		view_dictionary["zoom"] = view.zoom;
		view_dictionary["offset_x"] = view.offset_x;
		view_dictionary["offset_y"] = view.offset_y;
		result["view"] = view_dictionary;
		return result;
	} catch (const std::exception &error) {
		set_error(String::utf8(error.what()));
		return Dictionary();
	} catch (...) {
		set_error("Face tracking read failed with an unknown exception");
		return Dictionary();
	}
}

void OpenVTFaceTrackingRuntime::reset_calibration() {
	if (state != nullptr && state->mailbox != nullptr) {
		state->mailbox->request_calibration_reset();
	}
}

Error OpenVTFaceTrackingRuntime::stop() {
	if (state == nullptr) {
		running = false;
		configured = false;
		return OK;
	}
	Error result = OK;
	try {
		if (state->control_server != nullptr) {
			state->control_server->stop();
		}
		if (state->runtime != nullptr) {
			state->runtime->stop();
			state->runtime->rethrow_if_failed();
		}
	} catch (const std::exception &error) {
		set_error(String::utf8(error.what()));
		result = FAILED;
	} catch (...) {
		set_error("Face tracking stop failed with an unknown exception");
		result = FAILED;
	}
	state.reset();
	running = false;
	configured = false;
	return result;
}

String OpenVTFaceTrackingRuntime::get_last_error() const {
	return last_error;
}

void OpenVTFaceTrackingRuntime::_bind_methods() {
	ClassDB::bind_method(D_METHOD(
			"configure",
			"source",
			"width",
			"height",
			"fps",
			"buffer_count",
			"detector_model",
			"landmark_model",
			"calibration_frames",
			"smoothing_tau_ms",
			"angle_x_sign",
			"angle_y_sign",
			"angle_z_sign",
			"control_bind_address",
			"control_port",
			"control_allowed_origin"),
			&OpenVTFaceTrackingRuntime::configure);
	ClassDB::bind_method(D_METHOD("start"), &OpenVTFaceTrackingRuntime::start);
	ClassDB::bind_method(D_METHOD("take_latest"), &OpenVTFaceTrackingRuntime::take_latest);
	ClassDB::bind_method(D_METHOD("reset_calibration"), &OpenVTFaceTrackingRuntime::reset_calibration);
	ClassDB::bind_method(D_METHOD("stop"), &OpenVTFaceTrackingRuntime::stop);
	ClassDB::bind_method(D_METHOD("get_last_error"), &OpenVTFaceTrackingRuntime::get_last_error);
}

void OpenVTFaceTrackingRuntime::set_error(const String &p_error) {
	last_error = p_error;
}
