#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>

namespace vtb {

/** @brief 标识一次视频源缓冲池会话；零值保留为无效代次。 */
class VideoStreamGeneration final {
public:
    explicit VideoStreamGeneration(std::uint64_t value);
    ~VideoStreamGeneration() = default;

    VideoStreamGeneration(const VideoStreamGeneration&) = default;
    VideoStreamGeneration& operator=(const VideoStreamGeneration&) = default;
    VideoStreamGeneration(VideoStreamGeneration&&) noexcept = default;
    VideoStreamGeneration& operator=(VideoStreamGeneration&&) noexcept = default;

    std::uint64_t value() const noexcept;

    bool operator==(const VideoStreamGeneration& other) const noexcept;
    bool operator!=(const VideoStreamGeneration& other) const noexcept;

private:
    std::uint64_t value_ = 0;
};

/** @brief 控制线程发送给帧消费线程的源生命周期动作。 */
enum class VideoStreamControlAction {
    begin_source,
    retire_source,
    shutdown_consumer,
};

/** @brief 一条带请求号和源代次的不可变控制请求。 */
class VideoStreamControlRequest final {
public:
    VideoStreamControlRequest(
        std::uint64_t request_id,
        VideoStreamControlAction action,
        VideoStreamGeneration generation);
    ~VideoStreamControlRequest() = default;

    VideoStreamControlRequest(const VideoStreamControlRequest&) = default;
    VideoStreamControlRequest& operator=(
        const VideoStreamControlRequest&) = default;
    VideoStreamControlRequest(VideoStreamControlRequest&&) noexcept = default;
    VideoStreamControlRequest& operator=(
        VideoStreamControlRequest&&) noexcept = default;

    std::uint64_t request_id() const noexcept;
    VideoStreamControlAction action() const noexcept;
    VideoStreamGeneration generation() const noexcept;

private:
    std::uint64_t request_id_ = 0;
    VideoStreamControlAction action_ =
        VideoStreamControlAction::shutdown_consumer;
    VideoStreamGeneration generation_;
};

/** @brief 消费线程对一条控制请求的完成或失败应答。 */
class VideoStreamControlReply final {
public:
    VideoStreamControlReply(
        VideoStreamControlRequest request,
        std::exception_ptr failure);
    ~VideoStreamControlReply() = default;

    VideoStreamControlReply(const VideoStreamControlReply&) = default;
    VideoStreamControlReply& operator=(
        const VideoStreamControlReply&) = default;
    VideoStreamControlReply(VideoStreamControlReply&&) noexcept = default;
    VideoStreamControlReply& operator=(
        VideoStreamControlReply&&) noexcept = default;

    const VideoStreamControlRequest& request() const noexcept;
    bool succeeded() const noexcept;
    void rethrow_if_failed() const;

private:
    VideoStreamControlRequest request_;
    std::exception_ptr failure_;
};

/**
 * @brief 在控制线程与帧消费线程之间传递不可丢失的请求和应答。
 *
 * 视频帧继续通过 LatestFrameSlot 使用 latest-only 策略；源开始、源退休和消费线程
 * 关闭命令由本协议独立排队，使用 request_id 配对应答。协议只描述生命周期，不依赖
 * V4L2、RGA 或 FacePipeline。
 */
class VideoStreamControlProtocol final {
public:
    VideoStreamControlProtocol() = default;
    ~VideoStreamControlProtocol() = default;

    VideoStreamControlProtocol(const VideoStreamControlProtocol&) = delete;
    VideoStreamControlProtocol& operator=(
        const VideoStreamControlProtocol&) = delete;
    VideoStreamControlProtocol(VideoStreamControlProtocol&&) = delete;
    VideoStreamControlProtocol& operator=(VideoStreamControlProtocol&&) = delete;

    VideoStreamControlReply exchange(
        VideoStreamControlAction action,
        VideoStreamGeneration generation);
    std::optional<VideoStreamControlRequest> wait_request();
    void complete(const VideoStreamControlRequest& request);
    void fail(
        const VideoStreamControlRequest& request,
        std::exception_ptr failure);
    void close() noexcept;

private:
    std::optional<VideoStreamControlReply> take_reply(
        std::uint64_t request_id);

    std::mutex mutex_;
    std::condition_variable request_condition_;
    std::condition_variable reply_condition_;
    std::deque<VideoStreamControlRequest> requests_;
    std::deque<VideoStreamControlReply> replies_;
    std::uint64_t next_request_id_ = 1;
    bool closed_ = false;
};

}  // namespace vtb
