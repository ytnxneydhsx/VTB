#pragma once

#include <optional>

#include "vtb/video/frame.hpp"

namespace vtb {

class VideoSource {
public:
    VideoSource() = default;
    virtual ~VideoSource() = default;

    VideoSource(const VideoSource&) = delete;
    VideoSource& operator=(const VideoSource&) = delete;
    VideoSource(VideoSource&&) = delete;
    VideoSource& operator=(VideoSource&&) = delete;

    virtual std::optional<DecodedFrameLease> read() = 0;
    virtual void request_stop() noexcept = 0;
    virtual void close() noexcept = 0;
};

}  // namespace vtb
