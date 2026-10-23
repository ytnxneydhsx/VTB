#pragma once

#include <cstdint>
#include <vector>

namespace vtb {

struct FrameMetadata {
    std::uint64_t sequence = 0;
    std::uint32_t driver_sequence = 0;
    std::int64_t timestamp_ns = 0;
    std::uint32_t v4l2_flags = 0;
    std::uint32_t pixel_format = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct Frame {
    FrameMetadata metadata;
    std::vector<std::uint8_t> data;
};

}  // namespace vtb
