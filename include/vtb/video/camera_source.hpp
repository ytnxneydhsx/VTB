#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "vtb/capture_config.hpp"
#include "vtb/video/video_source.hpp"

namespace vtb {

class V4l2BufferPool;

class CameraSource final : public VideoSource {
public:
    explicit CameraSource(CaptureConfig config, std::uint32_t buffer_count = 4);
    ~CameraSource() override;

    CameraSource(const CameraSource&) = delete;
    CameraSource& operator=(const CameraSource&) = delete;
    CameraSource(CameraSource&&) = delete;
    CameraSource& operator=(CameraSource&&) = delete;

    std::optional<DecodedFrameLease> read() override;
    void request_stop() noexcept override;
    void close() noexcept override;

    const NegotiatedCaptureConfig& negotiated_config() const noexcept;

private:
    NegotiatedCaptureConfig negotiated_config_;
    std::shared_ptr<V4l2BufferPool> buffer_pool_;
};

}  // namespace vtb
