#include "vtb/face/face_tracking_service.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <time.h>
#include <utility>

#include "vtb/pipeline/pipeline_runtime.hpp"

namespace vtb {
namespace {

constexpr float kViewZoomMinimum = 0.5F;
constexpr float kViewZoomMaximum = 6.0F;
constexpr float kViewOffsetMinimum = -2.0F;
constexpr float kViewOffsetMaximum = 2.0F;

std::uint64_t monotonic_now_ns() {
    timespec timestamp{};
    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0) {
        throw std::runtime_error("clock_gettime(CLOCK_MONOTONIC) failed");
    }
    return static_cast<std::uint64_t>(timestamp.tv_sec) * 1000000000ULL +
        static_cast<std::uint64_t>(timestamp.tv_nsec);
}

class FaceParameterMailbox final {
public:
    explicit FaceParameterMailbox(Live2DMappingConfig config)
        : mapper_(config), calibration_required_(config.calibration_frames) {}
    ~FaceParameterMailbox() = default;

    FaceParameterMailbox(const FaceParameterMailbox&) = delete;
    FaceParameterMailbox& operator=(const FaceParameterMailbox&) = delete;
    FaceParameterMailbox(FaceParameterMailbox&&) = delete;
    FaceParameterMailbox& operator=(FaceParameterMailbox&&) = delete;

    void publish(const std::optional<FaceTrackingResult>& result) {
        const std::uint64_t requested_reset =
            reset_generation_.load(std::memory_order_acquire);
        if (requested_reset != applied_reset_generation_) {
            mapper_.reset_calibration();
            applied_reset_generation_ = requested_reset;
            reset_pending_.store(false, std::memory_order_release);
        }
        const FaceTrackingResult* face = nullptr;
        if (result.has_value()) {
            face = &result.value();
        }
        Live2DMappingResult mapped = mapper_.update(face, monotonic_now_ns());
        std::lock_guard<std::mutex> lock(mutex_);
        latest_ = std::move(mapped);
        ++generation_;
    }

    std::optional<FaceTrackingSnapshot> read() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!latest_.has_value()) {
            return std::nullopt;
        }
        return FaceTrackingSnapshot(generation_, latest_.value(), view_);
    }

    void request_calibration_reset() noexcept {
        reset_pending_.store(true, std::memory_order_release);
        reset_generation_.fetch_add(1, std::memory_order_release);
    }

    bool set_view(const FaceTrackingViewState& view) noexcept {
        if (!std::isfinite(view.zoom()) || !std::isfinite(view.offset_x()) ||
            !std::isfinite(view.offset_y()) ||
            view.zoom() < kViewZoomMinimum || view.zoom() > kViewZoomMaximum ||
            view.offset_x() < kViewOffsetMinimum ||
            view.offset_x() > kViewOffsetMaximum ||
            view.offset_y() < kViewOffsetMinimum ||
            view.offset_y() > kViewOffsetMaximum) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        view_ = view;
        ++generation_;
        return true;
    }

    std::string health_json() const {
        std::lock_guard<std::mutex> lock(mutex_);
        bool calibrated = false;
        bool tracking_valid = false;
        std::size_t calibration_progress = 0;
        float confidence = 0.0F;
        if (latest_.has_value()) {
            calibrated = latest_->calibrated();
            tracking_valid = latest_->tracking_valid();
            calibration_progress = latest_->calibration_progress();
            confidence = latest_->confidence();
        }
        if (reset_pending_.load(std::memory_order_acquire)) {
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
        body << ",\"required\":" << calibration_required_;
        body << ",\"confidence\":" << confidence;
        body << ",\"view\":{\"zoom\":" << view_.zoom();
        body << ",\"offset_x\":" << view_.offset_x();
        body << ",\"offset_y\":" << view_.offset_y() << "}}";
        return body.str();
    }

private:
    mutable std::mutex mutex_;
    Live2DFaceMapper mapper_;
    std::size_t calibration_required_ = 0;
    std::optional<Live2DMappingResult> latest_;
    std::atomic<std::uint64_t> reset_generation_{0};
    std::atomic<bool> reset_pending_{false};
    std::uint64_t applied_reset_generation_ = 0;
    std::uint64_t generation_ = 0;
    FaceTrackingViewState view_;
};

class Live2DControlServer final {
public:
    Live2DControlServer(
        FaceTrackingControlConfig config,
        std::shared_ptr<FaceParameterMailbox> mailbox)
        : config_(std::move(config)), mailbox_(std::move(mailbox)) {
        if (config_.bind_address.empty() || config_.port < 1 ||
            config_.port > 65535 || !mailbox_) {
            throw std::invalid_argument(
                "Live2D control server configuration is invalid");
        }
    }

    ~Live2DControlServer() {
        stop();
    }

    Live2DControlServer(const Live2DControlServer&) = delete;
    Live2DControlServer& operator=(const Live2DControlServer&) = delete;
    Live2DControlServer(Live2DControlServer&&) = delete;
    Live2DControlServer& operator=(Live2DControlServer&&) = delete;

    void start() {
        if (worker_.joinable()) {
            return;
        }
        const int created_socket = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (created_socket < 0) {
            throw std::runtime_error("Unable to create Live2D control socket");
        }
        int reuse_address = 1;
        setsockopt(
            created_socket,
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuse_address,
            sizeof(reuse_address));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(config_.port));
        if (inet_pton(
                AF_INET,
                config_.bind_address.c_str(),
                &address.sin_addr) != 1) {
            close(created_socket);
            throw std::invalid_argument(
                "Live2D control bind address must be an IPv4 address");
        }
        if (bind(
                created_socket,
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) != 0 ||
            listen(created_socket, 8) != 0) {
            const int saved_errno = errno;
            close(created_socket);
            throw std::runtime_error(
                "Unable to bind Live2D control socket: " +
                std::string(std::strerror(saved_errno)));
        }
        listen_socket_.store(created_socket, std::memory_order_release);
        stop_requested_.store(false, std::memory_order_release);
        worker_ = std::thread([this]() { run(); });
    }

    void stop() noexcept {
        request_stop();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    void request_stop() noexcept {
        stop_requested_.store(true, std::memory_order_release);
        const int socket_to_close =
            listen_socket_.exchange(-1, std::memory_order_acq_rel);
        if (socket_to_close >= 0) {
            shutdown(socket_to_close, SHUT_RDWR);
            close(socket_to_close);
        }
    }

private:
    static std::string header_value(
        const std::string& request,
        const std::string& name) {
        const std::string needle = "\r\n" + name + ":";
        const std::size_t position = request.find(needle);
        if (position == std::string::npos) {
            return std::string();
        }
        std::size_t value_start = position + needle.size();
        while (value_start < request.size() && request[value_start] == ' ') {
            ++value_start;
        }
        const std::size_t value_end = request.find("\r\n", value_start);
        if (value_end == std::string::npos) {
            return std::string();
        }
        return request.substr(value_start, value_end - value_start);
    }

    static bool parse_json_number(
        const std::string& body,
        const std::string& name,
        double& value) noexcept {
        const std::string key = "\"" + name + "\"";
        const std::size_t key_position = body.find(key);
        if (key_position == std::string::npos) {
            return false;
        }
        const std::size_t colon = body.find(':', key_position + key.size());
        if (colon == std::string::npos) {
            return false;
        }
        const char* number_start = body.c_str() + colon + 1U;
        char* number_end = nullptr;
        errno = 0;
        const double parsed = std::strtod(number_start, &number_end);
        if (number_start == number_end || errno == ERANGE ||
            !std::isfinite(parsed)) {
            return false;
        }
        value = parsed;
        return true;
    }

    bool origin_is_allowed(const std::string& request) const {
        if (config_.allowed_origin.empty()) {
            return true;
        }
        const std::string origin = header_value(request, "Origin");
        if (origin.empty()) {
            return true;
        }
        return origin == config_.allowed_origin;
    }

    std::string cors_headers(const std::string& request) const {
        if (config_.allowed_origin.empty()) {
            return std::string();
        }
        if (header_value(request, "Origin") != config_.allowed_origin) {
            return std::string();
        }
        return "Access-Control-Allow-Origin: " + config_.allowed_origin +
            "\r\nVary: Origin\r\n";
    }

    void send_response(
        int client,
        const std::string& request,
        int status,
        const std::string& reason,
        const std::string& body,
        const std::string& extra_headers = std::string()) const noexcept {
        std::ostringstream response;
        response << "HTTP/1.1 " << status << " " << reason << "\r\n";
        response << cors_headers(request);
        response << extra_headers;
        response << "Cache-Control: no-store\r\n";
        response << "Content-Type: application/json; charset=utf-8\r\n";
        response << "Content-Length: " << body.size() << "\r\n";
        response << "Connection: close\r\n\r\n" << body;
        const std::string encoded = response.str();
        std::size_t sent = 0;
        while (sent < encoded.size()) {
            const ssize_t result = send(
                client,
                encoded.data() + sent,
                encoded.size() - sent,
                MSG_NOSIGNAL);
            if (result <= 0) {
                return;
            }
            sent += static_cast<std::size_t>(result);
        }
    }

    std::string read_request(int client) const {
        timeval timeout{};
        timeout.tv_sec = 1;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        std::string request;
        request.reserve(2048);
        std::size_t expected_size = 0;
        while (request.size() < 8192U) {
            char buffer[1024];
            const ssize_t received = recv(client, buffer, sizeof(buffer), 0);
            if (received <= 0) {
                break;
            }
            request.append(buffer, static_cast<std::size_t>(received));
            const std::size_t header_end = request.find("\r\n\r\n");
            if (header_end == std::string::npos) {
                continue;
            }
            if (expected_size == 0) {
                const std::string content_length =
                    header_value(request, "Content-Length");
                std::size_t body_size = 0;
                if (!content_length.empty()) {
                    body_size = static_cast<std::size_t>(
                        std::stoul(content_length));
                }
                expected_size = header_end + 4U + body_size;
            }
            if (request.size() >= expected_size) {
                break;
            }
        }
        return request;
    }

    void handle_client(int client) noexcept {
        try {
            const std::string request = read_request(client);
            const std::size_t first_line_end = request.find("\r\n");
            const std::size_t header_end = request.find("\r\n\r\n");
            if (first_line_end == std::string::npos ||
                header_end == std::string::npos) {
                send_response(
                    client,
                    request,
                    400,
                    "Bad Request",
                    "{\"error\":\"invalid request\"}");
                return;
            }
            const std::string first_line = request.substr(0, first_line_end);
            if (first_line == "GET /health HTTP/1.1") {
                send_response(
                    client, request, 200, "OK", mailbox_->health_json());
                return;
            }
            if (first_line == "OPTIONS /recalibrate HTTP/1.1" ||
                first_line == "OPTIONS /view HTTP/1.1") {
                if (!origin_is_allowed(request)) {
                    send_response(
                        client,
                        request,
                        403,
                        "Forbidden",
                        "{\"error\":\"origin is not allowed\"}");
                    return;
                }
                send_response(
                    client,
                    request,
                    204,
                    "No Content",
                    std::string(),
                    "Access-Control-Allow-Methods: POST, OPTIONS\r\n"
                    "Access-Control-Allow-Headers: Content-Type\r\n");
                return;
            }
            if (!origin_is_allowed(request)) {
                send_response(
                    client,
                    request,
                    403,
                    "Forbidden",
                    "{\"error\":\"origin is not allowed\"}");
                return;
            }
            if (first_line == "POST /recalibrate HTTP/1.1") {
                mailbox_->request_calibration_reset();
                send_response(
                    client, request, 202, "Accepted", mailbox_->health_json());
                return;
            }
            if (first_line == "POST /view HTTP/1.1") {
                const std::string body = request.substr(header_end + 4U);
                double zoom = 0.0;
                double offset_x = 0.0;
                double offset_y = 0.0;
                if (!parse_json_number(body, "zoom", zoom) ||
                    !parse_json_number(body, "offset_x", offset_x) ||
                    !parse_json_number(body, "offset_y", offset_y) ||
                    !mailbox_->set_view(FaceTrackingViewState(
                        static_cast<float>(zoom),
                        static_cast<float>(offset_x),
                        static_cast<float>(offset_y)))) {
                    send_response(
                        client,
                        request,
                        400,
                        "Bad Request",
                        "{\"error\":\"invalid view\"}");
                    return;
                }
                send_response(
                    client, request, 200, "OK", mailbox_->health_json());
                return;
            }
            send_response(
                client,
                request,
                404,
                "Not Found",
                "{\"error\":\"not found\"}");
        } catch (...) {
            send_response(
                client,
                std::string(),
                400,
                "Bad Request",
                "{\"error\":\"invalid request\"}");
        }
    }

    void run() noexcept {
        while (!stop_requested_.load(std::memory_order_acquire)) {
            const int current_socket =
                listen_socket_.load(std::memory_order_acquire);
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
            const int client = accept4(
                current_socket, nullptr, nullptr, SOCK_CLOEXEC);
            if (client < 0) {
                continue;
            }
            handle_client(client);
            shutdown(client, SHUT_RDWR);
            close(client);
        }
    }

    FaceTrackingControlConfig config_;
    std::shared_ptr<FaceParameterMailbox> mailbox_;
    std::atomic<int> listen_socket_{-1};
    std::atomic<bool> stop_requested_{false};
    std::thread worker_;
};

}  // namespace

FaceTrackingViewState::FaceTrackingViewState(
    float zoom,
    float offset_x,
    float offset_y)
    : zoom_(zoom), offset_x_(offset_x), offset_y_(offset_y) {}

float FaceTrackingViewState::zoom() const noexcept {
    return zoom_;
}

float FaceTrackingViewState::offset_x() const noexcept {
    return offset_x_;
}

float FaceTrackingViewState::offset_y() const noexcept {
    return offset_y_;
}

FaceTrackingSnapshot::FaceTrackingSnapshot(
    std::uint64_t generation,
    Live2DMappingResult mapping,
    FaceTrackingViewState view)
    : generation_(generation),
      mapping_(std::move(mapping)),
      view_(std::move(view)) {}

std::uint64_t FaceTrackingSnapshot::generation() const noexcept {
    return generation_;
}

const Live2DMappingResult& FaceTrackingSnapshot::mapping() const noexcept {
    return mapping_;
}

const FaceTrackingViewState& FaceTrackingSnapshot::view() const noexcept {
    return view_;
}

class FaceTrackingService::Impl final {
public:
    Impl(
        std::unique_ptr<VideoSource> video_input,
        std::unique_ptr<FacePipeline> face_pipeline,
        Live2DMappingConfig mapping_config,
        FaceTrackingControlConfig control_config)
        : mailbox_(std::make_shared<FaceParameterMailbox>(mapping_config)),
          runtime_(std::make_unique<PipelineRuntime>(
              std::move(video_input),
              std::move(face_pipeline),
              [mailbox = mailbox_](
                  const std::optional<FaceTrackingResult>& result) {
                  mailbox->publish(result);
              })),
          control_server_(std::make_unique<Live2DControlServer>(
              std::move(control_config), mailbox_)) {}

    ~Impl() {
        shutdown_noexcept();
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void start() {
        if (started_) {
            return;
        }
        runtime_->start();
        try {
            control_server_->start();
        } catch (...) {
            runtime_->stop();
            throw;
        }
        started_ = true;
    }

    void request_stop() noexcept {
        control_server_->request_stop();
        runtime_->request_stop();
    }

    void stop() {
        control_server_->stop();
        runtime_->stop();
        runtime_->rethrow_if_failed();
        started_ = false;
    }

    void rethrow_if_failed() const {
        runtime_->rethrow_if_failed();
    }

    std::optional<FaceTrackingSnapshot> take_latest() {
        runtime_->rethrow_if_failed();
        std::optional<FaceTrackingSnapshot> snapshot = mailbox_->read();
        if (!snapshot.has_value() ||
            snapshot->generation() == delivered_generation_) {
            return std::nullopt;
        }
        delivered_generation_ = snapshot->generation();
        return snapshot;
    }

private:
    void shutdown_noexcept() noexcept {
        try {
            request_stop();
            runtime_->stop();
        } catch (...) {
        }
        started_ = false;
    }

    std::shared_ptr<FaceParameterMailbox> mailbox_;
    std::unique_ptr<PipelineRuntime> runtime_;
    std::unique_ptr<Live2DControlServer> control_server_;
    std::uint64_t delivered_generation_ = 0;
    bool started_ = false;
};

FaceTrackingService::FaceTrackingService(
    std::unique_ptr<VideoSource> video_input,
    std::unique_ptr<FacePipeline> face_pipeline,
    Live2DMappingConfig mapping_config,
    FaceTrackingControlConfig control_config)
    : impl_(std::make_unique<Impl>(
          std::move(video_input),
          std::move(face_pipeline),
          mapping_config,
          std::move(control_config))) {}

FaceTrackingService::~FaceTrackingService() = default;

void FaceTrackingService::start() {
    impl_->start();
}

void FaceTrackingService::request_stop() noexcept {
    impl_->request_stop();
}

void FaceTrackingService::stop() {
    impl_->stop();
}

void FaceTrackingService::rethrow_if_failed() const {
    impl_->rethrow_if_failed();
}

std::optional<FaceTrackingSnapshot> FaceTrackingService::take_latest() {
    return impl_->take_latest();
}

}  // namespace vtb
