#include "live2d_control_server.hpp"
#include "pipeline/face_tracking_stage.hpp"

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
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>


namespace vtb {

class Live2DControlServer::Impl final {
public:
    Impl(
        ControlConfiguration config,
        FaceTrackingStage& tracking_stage)
        : config_(std::move(config)), tracking_stage_(tracking_stage) {
        if (config_.bind_address.empty() || config_.port < 1) {
            throw std::invalid_argument(
                "Live2D control server configuration is invalid");
        }
    }

    ~Impl() {
        stop();
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

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
        listen_socket_.store(created_socket);
        stop_requested_.store(false);
        worker_ = std::thread([this]() { run(); });
    }

    void stop() noexcept {
        request_stop();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    void request_stop() noexcept {
        stop_requested_.store(true);
        const int socket_to_close =
            listen_socket_.exchange(-1);
        if (socket_to_close >= 0) {
            shutdown(socket_to_close, SHUT_RDWR);
            close(socket_to_close);
        }
    }

private:
    std::string health_json() const {
        const Live2DTrackingStatus status = tracking_stage_.status();
        std::ostringstream body;
        body << "{\"state\":\"running\",\"calibrated\":";
        if (status.calibrated) {
            body << "true";
        } else {
            body << "false";
        }
        body << ",\"tracking_valid\":";
        if (status.tracking_valid) {
            body << "true";
        } else {
            body << "false";
        }
        body << ",\"progress\":" << status.calibration_progress;
        body << ",\"required\":" << status.calibration_required;
        body << ",\"confidence\":" << status.confidence;
        body << ",\"view\":{\"zoom\":" << status.view.zoom();
        body << ",\"offset_x\":" << status.view.offset_x();
        body << ",\"offset_y\":" << status.view.offset_y() << "}}";
        return body.str();
    }

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
                    client, request, 200, "OK", health_json());
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
                tracking_stage_.request_calibration_reset();
                send_response(
                    client, request, 202, "Accepted", health_json());
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
                    !tracking_stage_.set_view(Live2DTrackingViewState(
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
                    client, request, 200, "OK", health_json());
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
        while (!stop_requested_.load()) {
            const int current_socket =
                listen_socket_.load();
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

    ControlConfiguration config_;
    FaceTrackingStage& tracking_stage_;
    std::atomic<int> listen_socket_{-1};
    std::atomic<bool> stop_requested_{false};
    std::thread worker_;
};

Live2DControlServer::Live2DControlServer(
    ControlConfiguration config,
    FaceTrackingStage& tracking_stage)
    : impl_(std::make_unique<Impl>(
          std::move(config), tracking_stage)) {}
Live2DControlServer::~Live2DControlServer() = default;
void Live2DControlServer::start() { impl_->start(); }
void Live2DControlServer::request_stop() noexcept { impl_->request_stop(); }
void Live2DControlServer::stop() noexcept { impl_->stop(); }

}  // namespace vtb
