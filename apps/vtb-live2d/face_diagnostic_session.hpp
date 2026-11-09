#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "vtb/config/vtb_application_config.hpp"
#include "vtb/face/face_types.hpp"
#include "vtb/output/encoded_video_sink.hpp"

namespace vtb {

/**
 * @brief 把最新人脸结果绘制成独立诊断画面并发布 H.264。
 *
 * 推理线程只复制 FaceTrackingResult；后台线程以 maximum_fps 为上限消费最新
 * 结果，旧结果会被替换。诊断线程不持有摄像头帧租约，也不会阻塞摄像头缓冲区
 * 归还。
 */
class FaceDiagnosticSession final {
public:
    FaceDiagnosticSession(
        FaceDiagnosticOutputConfiguration config,
        std::uint32_t frame_width,
        std::uint32_t frame_height,
        std::string dma_heap_path,
        EncodedVideoSink output_sink);
    ~FaceDiagnosticSession();

    FaceDiagnosticSession(const FaceDiagnosticSession&) = delete;
    FaceDiagnosticSession& operator=(const FaceDiagnosticSession&) = delete;
    FaceDiagnosticSession(FaceDiagnosticSession&&) = delete;
    FaceDiagnosticSession& operator=(FaceDiagnosticSession&&) = delete;

    void start();
    void publish(const std::optional<FaceTrackingResult>& result);
    void request_stop() noexcept;
    void stop();
    void rethrow_if_failed() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vtb
