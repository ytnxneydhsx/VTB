#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace vtb {

enum class MppVideoCodec {
    h264,
    mjpeg,
};

class MppEncodedPacket final {
public:
    MppEncodedPacket() = default;
    MppEncodedPacket(
        void* packet,
        std::uint64_t pts_ns,
        std::uint64_t dts_ns,
        bool keyframe) noexcept;
    ~MppEncodedPacket();

    MppEncodedPacket(const MppEncodedPacket&) = delete;
    MppEncodedPacket& operator=(const MppEncodedPacket&) = delete;
    MppEncodedPacket(MppEncodedPacket&& other) noexcept;
    MppEncodedPacket& operator=(MppEncodedPacket&& other) noexcept;

    const std::uint8_t* data() const noexcept;
    std::size_t size() const noexcept;
    std::uint64_t pts_ns() const noexcept;
    std::uint64_t dts_ns() const noexcept;
    bool keyframe() const noexcept;

private:
    void reset() noexcept;

    void* packet_ = nullptr;
    std::uint64_t pts_ns_ = 0;
    std::uint64_t dts_ns_ = 0;
    bool keyframe_ = false;
};

/** @brief 宿主侧单路 Rockchip MPP 编码会话。 */
class MppEncoderSession final {
public:
    MppEncoderSession();
    ~MppEncoderSession();

    MppEncoderSession(const MppEncoderSession&) = delete;
    MppEncoderSession& operator=(const MppEncoderSession&) = delete;
    MppEncoderSession(MppEncoderSession&&) = delete;
    MppEncoderSession& operator=(MppEncoderSession&&) = delete;

    void initialize(
        MppVideoCodec codec,
        int input_fd,
        std::size_t input_size,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t horizontal_stride,
        std::uint32_t vertical_stride,
        std::uint32_t frames_per_second);
    std::vector<std::uint8_t> h264_codec_configuration();
    MppEncodedPacket encode(
        std::uint64_t pts_ns,
        bool end_of_stream);
    void shutdown() noexcept;
    std::size_t encoded_bytes() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vtb
