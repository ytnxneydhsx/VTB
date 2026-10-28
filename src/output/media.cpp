#include "vtb/output/media.hpp"

#include <stdexcept>
#include <utility>

namespace vtb {

StreamDescriptor::StreamDescriptor(
    MediaType type, std::string id, std::uint32_t version, std::string format,
    std::uint32_t width, std::uint32_t height, std::uint32_t frame_rate_num,
    std::uint32_t frame_rate_den, bool encoded,
    std::vector<std::uint8_t> codec_config, std::uint32_t sample_rate,
    std::uint32_t channels)
    : type_(type), id_(std::move(id)), version_(version),
      format_(std::move(format)), width_(width), height_(height),
      frame_rate_num_(frame_rate_num), frame_rate_den_(frame_rate_den),
      encoded_(encoded), codec_config_(std::move(codec_config)),
      sample_rate_(sample_rate), channels_(channels) {
    validate();
}

StreamDescriptor StreamDescriptor::video(
    std::string id, std::uint32_t version, std::string format,
    std::uint32_t width, std::uint32_t height, std::uint32_t frame_rate_num,
    std::uint32_t frame_rate_den, bool encoded,
    std::vector<std::uint8_t> codec_config) {
    return StreamDescriptor(MediaType::video, std::move(id), version,
                            std::move(format), width, height, frame_rate_num,
                            frame_rate_den, encoded, std::move(codec_config), 0, 0);
}

StreamDescriptor StreamDescriptor::audio(std::string id, std::uint32_t version,
                                         std::string sample_format,
                                         std::uint32_t sample_rate,
                                         std::uint32_t channels, bool encoded,
                                         std::vector<std::uint8_t> codec_config) {
    return StreamDescriptor(MediaType::audio, std::move(id), version,
                            std::move(sample_format), 0, 0, 0, 0, encoded,
                            std::move(codec_config),
                            sample_rate, channels);
}

MediaType StreamDescriptor::type() const noexcept { return type_; }
const std::string& StreamDescriptor::id() const noexcept { return id_; }
std::uint32_t StreamDescriptor::version() const noexcept { return version_; }
const std::string& StreamDescriptor::format() const noexcept { return format_; }
std::uint32_t StreamDescriptor::width() const noexcept { return width_; }
std::uint32_t StreamDescriptor::height() const noexcept { return height_; }
std::uint32_t StreamDescriptor::frame_rate_num() const noexcept { return frame_rate_num_; }
std::uint32_t StreamDescriptor::frame_rate_den() const noexcept { return frame_rate_den_; }
bool StreamDescriptor::encoded() const noexcept { return encoded_; }
const std::vector<std::uint8_t>& StreamDescriptor::codec_config() const noexcept {
    return codec_config_;
}
std::uint32_t StreamDescriptor::sample_rate() const noexcept { return sample_rate_; }
std::uint32_t StreamDescriptor::channels() const noexcept { return channels_; }

void StreamDescriptor::validate() const {
    if (id_.empty()) {
        throw std::invalid_argument("stream id must not be empty");
    }
    if (version_ == 0) {
        throw std::invalid_argument("stream version must be positive");
    }
    if (format_.empty()) {
        throw std::invalid_argument("stream format must not be empty");
    }
    if (type_ == MediaType::video) {
        if (width_ == 0 || height_ == 0 || frame_rate_num_ == 0 ||
            frame_rate_den_ == 0 || sample_rate_ != 0 || channels_ != 0) {
            throw std::invalid_argument("invalid video stream descriptor");
        }
        if (!encoded_ && !codec_config_.empty()) {
            throw std::invalid_argument("raw video cannot carry codec configuration");
        }
        return;
    }
    if (sample_rate_ == 0 || channels_ == 0 || width_ != 0 || height_ != 0 ||
        frame_rate_num_ != 0 || frame_rate_den_ != 0 ||
        (!encoded_ && !codec_config_.empty())) {
        throw std::invalid_argument("invalid audio stream descriptor");
    }
}

MediaPacket::MediaPacket(
    std::string stream_id,
    std::shared_ptr<const std::vector<std::uint8_t>> payload,
    std::uint64_t pts_ns, std::uint64_t dts_ns, std::uint64_t duration_ns,
    std::uint64_t sequence, bool keyframe, bool discontinuity)
    : stream_id_(std::move(stream_id)), payload_(std::move(payload)),
      pts_ns_(pts_ns), dts_ns_(dts_ns), duration_ns_(duration_ns),
      sequence_(sequence), keyframe_(keyframe), discontinuity_(discontinuity) {
    if (stream_id_.empty()) {
        throw std::invalid_argument("packet stream id must not be empty");
    }
    if (!payload_) {
        throw std::invalid_argument("packet payload must not be null");
    }
    if (payload_->empty()) {
        throw std::invalid_argument("packet payload must not be empty");
    }
    if (duration_ns_ == 0) {
        throw std::invalid_argument("packet duration must be positive");
    }
}

const std::string& MediaPacket::stream_id() const noexcept { return stream_id_; }
const std::shared_ptr<const std::vector<std::uint8_t>>& MediaPacket::payload() const noexcept {
    return payload_;
}
std::uint64_t MediaPacket::pts_ns() const noexcept { return pts_ns_; }
std::uint64_t MediaPacket::dts_ns() const noexcept { return dts_ns_; }
std::uint64_t MediaPacket::duration_ns() const noexcept { return duration_ns_; }
std::uint64_t MediaPacket::sequence() const noexcept { return sequence_; }
bool MediaPacket::keyframe() const noexcept { return keyframe_; }
bool MediaPacket::discontinuity() const noexcept { return discontinuity_; }

MediaPacket MediaPacket::with_discontinuity() const {
    return MediaPacket(stream_id_, payload_, pts_ns_, dts_ns_, duration_ns_,
                       sequence_, keyframe_, true);
}

}  // namespace vtb
