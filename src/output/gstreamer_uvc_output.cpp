#include "vtb/output/gstreamer_uvc_output.hpp"

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace vtb {
namespace {

[[noreturn]] void throw_system_error(const std::string& operation, int error) {
    throw std::system_error(error, std::generic_category(), operation);
}

void close_fd(int& fd) noexcept {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

void drain_eventfd(int fd) noexcept {
    if (fd < 0) {
        return;
    }
    std::uint64_t value = 0;
    while (read(fd, &value, sizeof(value)) == static_cast<ssize_t>(sizeof(value))) {
    }
}

std::string normalized_format(std::string value) {
    for (char& character : value) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return value;
}

void require_child_running(pid_t child_pid) {
    int status = 0;
    const pid_t result = waitpid(child_pid, &status, WNOHANG);
    if (result < 0) {
        throw_system_error("waitpid GStreamer UVC process", errno);
    }
    if (result == child_pid) {
        throw std::runtime_error(
            "GStreamer UVC process exited before accepting video frames");
    }
}

std::vector<std::string> command_arguments(
    const GstreamerUvcOutputConfig& config,
    const StreamDescriptor& descriptor) {
    std::vector<std::string> arguments;
    arguments.emplace_back("/usr/bin/env");
    if (!config.library_path.empty()) {
        arguments.emplace_back("LD_LIBRARY_PATH=" + config.library_path);
    }
    if (!config.plugin_path.empty()) {
        arguments.emplace_back("GST_PLUGIN_PATH=" + config.plugin_path);
        arguments.emplace_back("GST_PLUGIN_SYSTEM_PATH=" + config.plugin_path);
    }
    arguments.push_back(config.gst_launch_path);
    arguments.emplace_back("-q");
    arguments.emplace_back("fdsrc");
    arguments.emplace_back("fd=0");
    arguments.emplace_back("do-timestamp=true");
    arguments.emplace_back("!");
    arguments.emplace_back(
        "image/jpeg,width=" + std::to_string(descriptor.width()) +
        ",height=" + std::to_string(descriptor.height()) +
        ",framerate=" + std::to_string(descriptor.frame_rate_num()) +
        "/" + std::to_string(descriptor.frame_rate_den()));
    arguments.emplace_back("!");
    arguments.emplace_back("jpegparse");
    arguments.emplace_back("!");
    arguments.emplace_back("queue");
    arguments.emplace_back("max-size-buffers=4");
    arguments.emplace_back("leaky=downstream");
    arguments.emplace_back("!");
    arguments.emplace_back("uvcsink");
    arguments.emplace_back("v4l2sink::device=" + config.device_path);
    return arguments;
}

std::vector<char*> command_argv(std::vector<std::string>& arguments) {
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (std::string& argument : arguments) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    return argv;
}

void wait_for_child(pid_t child_pid, std::chrono::milliseconds timeout) noexcept {
    if (child_pid < 0) {
        return;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        const pid_t result = waitpid(child_pid, &status, WNOHANG);
        if (result == child_pid || (result < 0 && errno == ECHILD)) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    kill(child_pid, SIGTERM);
    const auto terminate_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < terminate_deadline) {
        int status = 0;
        const pid_t result = waitpid(child_pid, &status, WNOHANG);
        if (result == child_pid || (result < 0 && errno == ECHILD)) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    kill(child_pid, SIGKILL);
    int status = 0;
    while (waitpid(child_pid, &status, 0) < 0 && errno == EINTR) {
    }
}

}  // namespace

GstreamerUvcOutput::GstreamerUvcOutput(GstreamerUvcOutputConfig config)
    : config_(std::move(config)) {
    if (config_.gst_launch_path.empty() || config_.device_path.empty()) {
        throw std::invalid_argument(
            "GStreamer launcher path and UVC device path are required");
    }
    if (config_.poll_timeout_ms <= 0 || config_.stop_timeout.count() <= 0) {
        throw std::invalid_argument("GStreamer UVC timeouts must be positive");
    }
    stop_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (stop_fd_ < 0) {
        throw_system_error("eventfd for GStreamer UVC stop", errno);
    }
}

GstreamerUvcOutput::~GstreamerUvcOutput() {
    stop();
    close_fd(stop_fd_);
}

bool GstreamerUvcOutput::supports(const StreamDescriptor& descriptor) const {
    if (descriptor.type() != MediaType::video || !descriptor.encoded()) {
        return false;
    }
    const std::string format = normalized_format(descriptor.format());
    return format == "mjpeg" || format == "mjpg";
}

void GstreamerUvcOutput::start(
    const std::vector<StreamDescriptor>& descriptors) {
    if (child_pid_ >= 0 || stream_fd_ >= 0) {
        throw std::logic_error("GStreamer UVC output is already started");
    }
    if (descriptors.size() != 1 || !supports(descriptors.front())) {
        throw std::invalid_argument(
            "GStreamer UVC output requires one encoded MJPEG video stream");
    }
    descriptors.front().validate();

    int sockets[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) < 0) {
        throw_system_error("socketpair for GStreamer UVC input", errno);
    }
    std::vector<std::string> arguments =
        command_arguments(config_, descriptors.front());
    std::vector<char*> argv = command_argv(arguments);
    drain_eventfd(stop_fd_);
    stop_requested_.store(false);

    const pid_t child = fork();
    if (child < 0) {
        const int error = errno;
        close(sockets[0]);
        close(sockets[1]);
        throw_system_error("fork GStreamer UVC process", error);
    }
    if (child == 0) {
        if (dup2(sockets[1], STDIN_FILENO) < 0) {
            _exit(126);
        }
        close(sockets[0]);
        close(sockets[1]);
        close(stop_fd_);
        execv(argv[0], argv.data());
        _exit(127);
    }

    close(sockets[1]);
    stream_fd_ = sockets[0];
    child_pid_ = child;
    stream_id_ = descriptors.front().id();
    const int flags = fcntl(stream_fd_, F_GETFL, 0);
    if (flags < 0 || fcntl(stream_fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
        const int error = errno;
        stop();
        throw_system_error("set nonblocking GStreamer UVC socket", error);
    }
    require_child_running(child_pid_);
}

void GstreamerUvcOutput::write(MediaPacket packet) {
    if (stop_requested_.load()) {
        throw std::runtime_error("GStreamer UVC output stop requested");
    }
    if (stream_fd_ < 0 || child_pid_ < 0 || packet.stream_id() != stream_id_) {
        throw std::logic_error("GStreamer UVC output is unavailable for this stream");
    }
    require_child_running(child_pid_);
    const std::vector<std::uint8_t>& payload = *packet.payload();
    std::size_t offset = 0;
    while (offset < payload.size()) {
        pollfd descriptors[2]{};
        descriptors[0].fd = stream_fd_;
        descriptors[0].events = POLLOUT;
        descriptors[1].fd = stop_fd_;
        descriptors[1].events = POLLIN;
        int result;
        do {
            result = poll(descriptors, 2, config_.poll_timeout_ms);
        } while (result < 0 && errno == EINTR);
        if (result < 0) {
            throw_system_error("poll GStreamer UVC input", errno);
        }
        if ((descriptors[1].revents & POLLIN) != 0) {
            throw std::runtime_error("GStreamer UVC output stop requested");
        }
        if (result == 0) {
            throw std::runtime_error("timed out writing an MJPEG frame to GStreamer");
        }
        if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            throw std::runtime_error("GStreamer UVC process closed its input");
        }
        if ((descriptors[0].revents & POLLOUT) == 0) {
            continue;
        }
        const ssize_t written = send(
            stream_fd_, payload.data() + offset, payload.size() - offset,
            MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            throw_system_error("write MJPEG frame to GStreamer", errno);
        }
        offset += static_cast<std::size_t>(written);
    }
}

void GstreamerUvcOutput::request_stop() noexcept {
    stop_requested_.store(true);
    if (stop_fd_ >= 0) {
        const std::uint64_t value = 1;
        const ssize_t ignored = ::write(stop_fd_, &value, sizeof(value));
        (void)ignored;
    }
}

void GstreamerUvcOutput::stop() noexcept {
    request_stop();
    close_fd(stream_fd_);
    const pid_t child = child_pid_;
    child_pid_ = -1;
    wait_for_child(child, config_.stop_timeout);
    stream_id_.clear();
    drain_eventfd(stop_fd_);
}

std::string GstreamerUvcOutput::name() const {
    return "gstreamer-uvc";
}

}  // namespace vtb
