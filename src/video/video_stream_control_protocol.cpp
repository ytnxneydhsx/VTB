#include "vtb/video/video_stream_control_protocol.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace vtb {

VideoStreamGeneration::VideoStreamGeneration(std::uint64_t value)
    : value_(value) {
    if (value_ == 0) {
        throw std::invalid_argument("Video stream generation must not be zero");
    }
}

std::uint64_t VideoStreamGeneration::value() const noexcept {
    return value_;
}

bool VideoStreamGeneration::operator==(
    const VideoStreamGeneration& other) const noexcept {
    return value_ == other.value_;
}

bool VideoStreamGeneration::operator!=(
    const VideoStreamGeneration& other) const noexcept {
    return !(*this == other);
}

VideoStreamControlRequest::VideoStreamControlRequest(
    std::uint64_t request_id,
    VideoStreamControlAction action,
    VideoStreamGeneration generation)
    : request_id_(request_id), action_(action), generation_(generation) {
    if (request_id_ == 0) {
        throw std::invalid_argument(
            "Video stream control request id must not be zero");
    }
}

std::uint64_t VideoStreamControlRequest::request_id() const noexcept {
    return request_id_;
}

VideoStreamControlAction VideoStreamControlRequest::action() const noexcept {
    return action_;
}

VideoStreamGeneration VideoStreamControlRequest::generation() const noexcept {
    return generation_;
}

VideoStreamControlReply::VideoStreamControlReply(
    VideoStreamControlRequest request,
    std::exception_ptr failure)
    : request_(std::move(request)), failure_(std::move(failure)) {}

const VideoStreamControlRequest& VideoStreamControlReply::request() const noexcept {
    return request_;
}

bool VideoStreamControlReply::succeeded() const noexcept {
    return !failure_;
}

void VideoStreamControlReply::rethrow_if_failed() const {
    if (failure_) {
        std::rethrow_exception(failure_);
    }
}

VideoStreamControlReply VideoStreamControlProtocol::exchange(
    VideoStreamControlAction action,
    VideoStreamGeneration generation) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (closed_) {
        throw std::logic_error("Video stream control protocol is closed");
    }
    const std::uint64_t request_id = next_request_id_;
    ++next_request_id_;
    requests_.emplace_back(request_id, action, generation);
    request_condition_.notify_one();

    while (true) {
        std::optional<VideoStreamControlReply> reply = take_reply(request_id);
        if (reply) {
            return std::move(*reply);
        }
        if (closed_) {
            throw std::runtime_error(
                "Video stream control protocol closed before reply");
        }
        reply_condition_.wait(lock);
    }
}

std::optional<VideoStreamControlRequest>
VideoStreamControlProtocol::wait_request() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (requests_.empty() && !closed_) {
        request_condition_.wait(lock);
    }
    if (requests_.empty()) {
        return std::nullopt;
    }
    VideoStreamControlRequest request = std::move(requests_.front());
    requests_.pop_front();
    return request;
}

void VideoStreamControlProtocol::complete(
    const VideoStreamControlRequest& request) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            return;
        }
        replies_.emplace_back(request, std::exception_ptr{});
    }
    reply_condition_.notify_all();
}

void VideoStreamControlProtocol::fail(
    const VideoStreamControlRequest& request,
    std::exception_ptr failure) {
    if (!failure) {
        failure = std::make_exception_ptr(std::runtime_error(
            "Video stream control request failed without an exception"));
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            return;
        }
        replies_.emplace_back(request, std::move(failure));
    }
    reply_condition_.notify_all();
}

void VideoStreamControlProtocol::close() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }
    request_condition_.notify_all();
    reply_condition_.notify_all();
}

std::optional<VideoStreamControlReply>
VideoStreamControlProtocol::take_reply(std::uint64_t request_id) {
    auto iterator = std::find_if(
        replies_.begin(), replies_.end(),
        [request_id](const VideoStreamControlReply& reply) {
            return reply.request().request_id() == request_id;
        });
    if (iterator == replies_.end()) {
        return std::nullopt;
    }
    VideoStreamControlReply reply = std::move(*iterator);
    replies_.erase(iterator);
    return reply;
}

}  // namespace vtb
