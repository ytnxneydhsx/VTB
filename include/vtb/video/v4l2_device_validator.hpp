#pragma once

#include "vtb/capture_config.hpp"

namespace vtb {

class V4l2DeviceValidator final {
public:
    NegotiatedCaptureConfig validate(const CaptureConfig& requested) const;
};

}  // namespace vtb
