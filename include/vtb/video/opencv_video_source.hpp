#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "vtb/video/video_source.hpp"

namespace vtb {

class OpenCvVideoSourceConfig {
public:
    std::string source = "/dev/video0";
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    std::uint32_t fps = 60;
    std::uint32_t buffer_count = 4;
    bool request_mjpeg = true;
};

class OpenCvVideoSourceState;

class OpenCvVideoSource final : public VideoSource {
public:
    explicit OpenCvVideoSource(OpenCvVideoSourceConfig config);
    ~OpenCvVideoSource() override;

    OpenCvVideoSource(const OpenCvVideoSource&) = delete;
    OpenCvVideoSource& operator=(const OpenCvVideoSource&) = delete;
    OpenCvVideoSource(OpenCvVideoSource&&) = delete;
    OpenCvVideoSource& operator=(OpenCvVideoSource&&) = delete;

    std::optional<DecodedFrameLease> read() override;
    void request_stop() noexcept override;
    void close() noexcept override;

private:
    std::unique_ptr<OpenCvVideoSourceState> state_;
};

}  // namespace vtb
