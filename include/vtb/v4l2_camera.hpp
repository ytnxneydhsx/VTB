#pragma once

#include <string>

#include "vtb/capture_config.hpp"

namespace vtb {

class V4l2Camera {
public:
    explicit V4l2Camera(CaptureConfig config);
    ~V4l2Camera();

    V4l2Camera(const V4l2Camera&) = delete;
    V4l2Camera& operator=(const V4l2Camera&) = delete;

    NegotiatedCaptureConfig open_and_configure();
    int fd() const noexcept;

private:
    CaptureConfig config_;
    int fd_ = -1;
};

std::string fourcc_to_string(std::uint32_t fourcc);

}  // namespace vtb
