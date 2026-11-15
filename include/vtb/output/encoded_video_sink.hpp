#pragma once

#include <cstdint>
#include <type_traits>

namespace vtb {

enum class EncodedVideoCodec : std::uint32_t {
    h264 = 1,
    mjpeg = 2,
};

// Borrowed encoded-video configuration. Valid only during configure().
class EncodedVideoStreamFormat {
public:
    EncodedVideoCodec codec = EncodedVideoCodec::h264;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t fps = 0;
    const std::uint8_t* codec_configuration = nullptr;
    std::uint64_t codec_configuration_size = 0;
};

// Borrowed encoded bytes. PTS/DTS use CLOCK_MONOTONIC nanoseconds.
class EncodedVideoPacketView {
public:
    EncodedVideoCodec codec = EncodedVideoCodec::h264;
    const std::uint8_t* data = nullptr;
    std::uint64_t size = 0;
    std::uint64_t pts_ns = 0;
    std::uint64_t dts_ns = 0;
    std::uint64_t duration_ns = 0;
    std::uint64_t sequence = 0;
    std::uint32_t keyframe = 0;
};

// ABI values only: no STL ownership or exceptions cross the shared library.
// configure runs once per enabled codec before the owner thread starts publishing it.
// Callbacks consume all borrowed data before returning. The host copies packet bytes
// once into its immutable shared payload; downstream queues only share or move it.
// Zero means accepted according to the sink's queue/drop policy, not necessarily
// written downstream.
// The host owns context and keeps it alive until the producer has joined its worker.
// This layout is versioned together with the OpenVT LibGodot bridge ABI.
class EncodedVideoSink {
public:
    void* context = nullptr;
    std::int32_t (*configure)(void*, const EncodedVideoStreamFormat*) noexcept = nullptr;
    std::int32_t (*publish)(void*, const EncodedVideoPacketView*) noexcept = nullptr;
};

static_assert(std::is_standard_layout<EncodedVideoStreamFormat>::value);
static_assert(std::is_trivially_copyable<EncodedVideoStreamFormat>::value);
static_assert(std::is_standard_layout<EncodedVideoPacketView>::value);
static_assert(std::is_trivially_copyable<EncodedVideoPacketView>::value);
static_assert(std::is_standard_layout<EncodedVideoSink>::value);
static_assert(std::is_trivially_copyable<EncodedVideoSink>::value);

}  // namespace vtb
