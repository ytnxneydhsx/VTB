#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "vtb/video/video_source.hpp"

namespace vtb {

class OpenCvVideoSourceConfig {
public:
    std::string device_path;
    std::uint32_t pixel_format = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t fps = 0;
    std::uint32_t buffer_count = 0;
};

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
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vtb
