#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vtb {

enum class MediaType {
    video,
    audio,
};

class StreamDescriptor {
public:
    static StreamDescriptor video(std::string id, std::uint32_t version,
                                  std::string format, std::uint32_t width,
                                  std::uint32_t height, std::uint32_t frame_rate_num,
                                  std::uint32_t frame_rate_den, bool encoded,
                                  std::vector<std::uint8_t> codec_config = {});
    static StreamDescriptor audio(std::string id, std::uint32_t version,
                                  std::string sample_format, std::uint32_t sample_rate,
                                  std::uint32_t channels, bool encoded = false,
                                  std::vector<std::uint8_t> codec_config = {});

    MediaType type() const noexcept;
    const std::string& id() const noexcept;
    std::uint32_t version() const noexcept;
    const std::string& format() const noexcept;
    std::uint32_t width() const noexcept;
    std::uint32_t height() const noexcept;
    std::uint32_t frame_rate_num() const noexcept;
    std::uint32_t frame_rate_den() const noexcept;
    bool encoded() const noexcept;
    const std::vector<std::uint8_t>& codec_config() const noexcept;
    std::uint32_t sample_rate() const noexcept;
    std::uint32_t channels() const noexcept;

    void validate() const;

private:
    StreamDescriptor(MediaType type, std::string id, std::uint32_t version,
                     std::string format, std::uint32_t width, std::uint32_t height,
                     std::uint32_t frame_rate_num, std::uint32_t frame_rate_den,
                     bool encoded, std::vector<std::uint8_t> codec_config,
                     std::uint32_t sample_rate, std::uint32_t channels);

    MediaType type_;
    std::string id_;
    std::uint32_t version_;
    std::string format_;
    std::uint32_t width_;
    std::uint32_t height_;
    std::uint32_t frame_rate_num_;
    std::uint32_t frame_rate_den_;
    bool encoded_;
    std::vector<std::uint8_t> codec_config_;
    std::uint32_t sample_rate_;
    std::uint32_t channels_;
};

class MediaPacket {
public:
    // PTS and DTS are nanoseconds on the CLOCK_MONOTONIC time base.
    MediaPacket(std::string stream_id,
                std::shared_ptr<const std::vector<std::uint8_t>> payload,
                std::uint64_t pts_ns, std::uint64_t dts_ns,
                std::uint64_t duration_ns, std::uint64_t sequence,
                bool keyframe = false, bool discontinuity = false);

    const std::string& stream_id() const noexcept;
    const std::shared_ptr<const std::vector<std::uint8_t>>& payload() const noexcept;
    std::uint64_t pts_ns() const noexcept;
    std::uint64_t dts_ns() const noexcept;
    std::uint64_t duration_ns() const noexcept;
    std::uint64_t sequence() const noexcept;
    bool keyframe() const noexcept;
    bool discontinuity() const noexcept;

    MediaPacket with_discontinuity() const;

private:
    std::string stream_id_;
    std::shared_ptr<const std::vector<std::uint8_t>> payload_;
    std::uint64_t pts_ns_;
    std::uint64_t dts_ns_;
    std::uint64_t duration_ns_;
    std::uint64_t sequence_;
    bool keyframe_;
    bool discontinuity_;
};

}  // namespace vtb
