#pragma once

#include <cstdint>
#include <memory>
#include <thread>

#include "vtb/video/video_frame_consumer.hpp"
#include "vtb/video/video_source.hpp"

namespace vtb {

class VideoProcessingRuntimeState;

/**
 * @brief 连接一个视频源和一个同步帧消费者的 latest-only 运行时。
 *
 * 采集线程从 VideoSource 读取帧并发布到容量为一的 LatestFrameSlot；消费线程取得
 * 最新帧并调用 VideoFrameConsumer::consume()。本类不依赖具体模型或结果类型。
 *
 * start()、request_stop()、wait()、stop() 和析构由拥有本对象的同一个控制线程
 * 调用。两个工作线程只访问 VideoProcessingRuntimeState，不访问本对象的生命周期
 * 函数。运行时在销毁状态对象前等待两个工作线程退出。
 */
class VideoProcessingRuntime {
public:
    VideoProcessingRuntime(
        std::unique_ptr<VideoSource> video_source,
        std::unique_ptr<VideoFrameConsumer> frame_consumer);
    ~VideoProcessingRuntime();

    VideoProcessingRuntime(const VideoProcessingRuntime&) = delete;
    VideoProcessingRuntime& operator=(const VideoProcessingRuntime&) = delete;
    VideoProcessingRuntime(VideoProcessingRuntime&&) = delete;
    VideoProcessingRuntime& operator=(VideoProcessingRuntime&&) = delete;

    void start();
    void request_stop() noexcept;
    void wait();
    void stop();
    void rethrow_if_failed() const;

    std::uint64_t published_frame_count() const noexcept;
    std::uint64_t replaced_frame_count() const noexcept;
    std::uint64_t processed_frame_count() const noexcept;

private:
    void join_threads();

    std::unique_ptr<VideoProcessingRuntimeState> state_;
    std::thread capture_thread_;
    std::thread consumer_thread_;
    bool started_ = false;
    bool stop_requested_ = false;
};

}  // namespace vtb
