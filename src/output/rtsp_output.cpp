#include "vtb/output/rtsp_output.hpp"

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libavutil/version.h>
}

namespace vtb {
namespace {

std::string normalized_format(const std::string& format) {
    std::string result = format;
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char value) {
        if (value >= 'A' && value <= 'Z') {
            return static_cast<char>(value - 'A' + 'a');
        }
        return static_cast<char>(value);
    });
    return result;
}

std::string ffmpeg_error(int error) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    av_strerror(error, buffer.data(), buffer.size());
    return std::string(buffer.data());
}

[[noreturn]] void throw_ffmpeg_error(const std::string& operation, int error) {
    throw std::runtime_error(operation + ": " + ffmpeg_error(error));
}

AVCodecID video_codec_id(const std::string& format) {
    const std::string value = normalized_format(format);
    if (value == "h264" || value == "avc") {
        return AV_CODEC_ID_H264;
    }
    if (value == "h265" || value == "hevc") {
        return AV_CODEC_ID_HEVC;
    }
    if (value == "mjpeg" || value == "mjpg") {
        return AV_CODEC_ID_MJPEG;
    }
    return AV_CODEC_ID_NONE;
}

AVCodecID audio_codec_id(const std::string& format) {
    const std::string value = normalized_format(format);
    if (value == "aac") {
        return AV_CODEC_ID_AAC;
    }
    if (value == "opus") {
        return AV_CODEC_ID_OPUS;
    }
    if (value == "pcm_s16le" || value == "s16le") {
        return AV_CODEC_ID_PCM_S16LE;
    }
    return AV_CODEC_ID_NONE;
}

void set_channel_layout(AVCodecParameters* parameters, std::uint32_t channels) {
#if LIBAVCODEC_VERSION_MAJOR >= 59
    av_channel_layout_default(&parameters->ch_layout, static_cast<int>(channels));
#else
    parameters->channels = static_cast<int>(channels);
    parameters->channel_layout = av_get_default_channel_layout(
        static_cast<int>(channels));
#endif
}

void set_codec_type(AVCodecParameters* parameters, MediaType type) {
    if (type == MediaType::video) {
        parameters->codec_type = AVMEDIA_TYPE_VIDEO;
    } else {
        parameters->codec_type = AVMEDIA_TYPE_AUDIO;
    }
}

AVCodecID descriptor_codec_id(const StreamDescriptor& descriptor) {
    if (descriptor.type() == MediaType::video) {
        return video_codec_id(descriptor.format());
    }
    return audio_codec_id(descriptor.format());
}

void set_padded_extradata(AVCodecParameters* parameters,
                          const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) {
        return;
    }
    if (bytes.size() > static_cast<std::size_t>(INT_MAX) - AV_INPUT_BUFFER_PADDING_SIZE) {
        throw std::invalid_argument("codec configuration is too large");
    }
    const std::size_t allocation_size = bytes.size() + AV_INPUT_BUFFER_PADDING_SIZE;
    auto* extradata = static_cast<std::uint8_t*>(av_mallocz(allocation_size));
    if (extradata == nullptr) {
        throw std::bad_alloc();
    }
    std::copy(bytes.begin(), bytes.end(), extradata);
    parameters->extradata = extradata;
    parameters->extradata_size = static_cast<int>(bytes.size());
}

std::int64_t monotonic_now_ns() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

}  // namespace

RtspOutput::RtspOutput(std::string url, std::chrono::milliseconds io_timeout)
    : url_(std::move(url)), io_timeout_(io_timeout) {
    if (url_.empty()) {
        throw std::invalid_argument("RTSP URL must not be empty");
    }
    if (io_timeout_.count() <= 0 ||
        io_timeout_.count() > std::numeric_limits<std::int64_t>::max() / 1000000) {
        throw std::invalid_argument("RTSP I/O timeout must be positive");
    }
}

RtspOutput::~RtspOutput() { stop(); }

bool RtspOutput::supports(const StreamDescriptor& descriptor) const {
    if (descriptor.type() == MediaType::video) {
        const AVCodecID codec_id = video_codec_id(descriptor.format());
        if (!descriptor.encoded() || codec_id == AV_CODEC_ID_NONE) {
            return false;
        }
        if (descriptor.width() > static_cast<std::uint32_t>(INT_MAX) ||
            descriptor.height() > static_cast<std::uint32_t>(INT_MAX) ||
            descriptor.frame_rate_num() > static_cast<std::uint32_t>(INT_MAX) ||
            descriptor.frame_rate_den() > static_cast<std::uint32_t>(INT_MAX)) {
            return false;
        }
        if ((codec_id == AV_CODEC_ID_H264 || codec_id == AV_CODEC_ID_HEVC) &&
            descriptor.codec_config().empty()) {
            return false;
        }
        return true;
    }
    const AVCodecID codec_id = audio_codec_id(descriptor.format());
    if (codec_id == AV_CODEC_ID_NONE) {
        return false;
    }
    if (descriptor.sample_rate() > static_cast<std::uint32_t>(INT_MAX) ||
        descriptor.channels() > static_cast<std::uint32_t>(INT_MAX)) {
        return false;
    }
    if (codec_id == AV_CODEC_ID_PCM_S16LE) {
        return !descriptor.encoded() && descriptor.codec_config().empty();
    }
    if (!descriptor.encoded() || descriptor.codec_config().empty()) {
        return false;
    }
    if (codec_id == AV_CODEC_ID_OPUS && descriptor.sample_rate() != 48000) {
        return false;
    }
    return true;
}

void RtspOutput::start(const std::vector<StreamDescriptor>& descriptors) {
    if (context_ != nullptr) {
        throw std::logic_error("RTSP output is already started");
    }
    if (descriptors.empty() || descriptors.size() > 2) {
        throw std::invalid_argument("RTSP output requires one or two streams");
    }
    bool has_video = false;
    bool has_audio = false;
    for (const StreamDescriptor& descriptor : descriptors) {
        descriptor.validate();
        if (!supports(descriptor)) {
            throw std::invalid_argument("unsupported RTSP stream format: " +
                                        descriptor.format());
        }
        if (descriptor.type() == MediaType::video) {
            if (has_video) {
                throw std::invalid_argument("RTSP output supports one video stream");
            }
            has_video = true;
        } else {
            if (has_audio) {
                throw std::invalid_argument("RTSP output supports one audio stream");
            }
            has_audio = true;
        }
    }

    owner_thread_ = std::this_thread::get_id();
    cancelled_.store(false, std::memory_order_release);
    set_operation_deadline();
    static std::once_flag network_init_flag;
    std::call_once(network_init_flag, [] {
        const int init_result = avformat_network_init();
        if (init_result < 0) {
            throw_ffmpeg_error("avformat_network_init", init_result);
        }
    });

    try {
        int result = avformat_alloc_output_context2(&context_, nullptr, "rtsp", url_.c_str());
        if (result < 0) {
            throw_ffmpeg_error("avformat_alloc_output_context2", result);
        }
        if (context_ == nullptr) {
            throw std::runtime_error("FFmpeg did not allocate an RTSP output context");
        }
        context_->interrupt_callback.callback = &RtspOutput::interrupt_callback;
        context_->interrupt_callback.opaque = this;

        for (const StreamDescriptor& descriptor : descriptors) {
            AVStream* stream = avformat_new_stream(context_, nullptr);
            if (stream == nullptr) {
                throw std::bad_alloc();
            }
            AVCodecParameters* parameters = stream->codecpar;
            set_codec_type(parameters, descriptor.type());
            parameters->codec_id = descriptor_codec_id(descriptor);
            parameters->codec_tag = 0;
            stream->time_base = AVRational{1, 1000000000};

            if (descriptor.type() == MediaType::video) {
                parameters->width = static_cast<int>(descriptor.width());
                parameters->height = static_cast<int>(descriptor.height());
                stream->avg_frame_rate = AVRational{
                    static_cast<int>(descriptor.frame_rate_num()),
                    static_cast<int>(descriptor.frame_rate_den())};
                if (parameters->codec_id == AV_CODEC_ID_H264 ||
                    parameters->codec_id == AV_CODEC_ID_HEVC) {
                    if (descriptor.codec_config().empty()) {
                        throw std::invalid_argument(
                            "H.264/H.265 RTSP output requires codec configuration before start");
                    }
                    set_padded_extradata(parameters, descriptor.codec_config());
                }
            } else {
                parameters->sample_rate = static_cast<int>(descriptor.sample_rate());
                set_channel_layout(parameters, descriptor.channels());
                if (parameters->codec_id == AV_CODEC_ID_PCM_S16LE) {
                    parameters->format = AV_SAMPLE_FMT_S16;
                } else if (parameters->codec_id == AV_CODEC_ID_AAC) {
                    set_padded_extradata(parameters, descriptor.codec_config());
                } else if (parameters->codec_id == AV_CODEC_ID_OPUS) {
                    parameters->sample_rate = 48000;
                    set_padded_extradata(parameters, descriptor.codec_config());
                }
            }
            stream_indexes_.emplace(descriptor.id(), stream->index);
        }

        AVDictionary* options = nullptr;
        result = av_dict_set(&options, "rtsp_transport", "tcp", 0);
        if (result < 0) {
            av_dict_free(&options);
            throw_ffmpeg_error("av_dict_set(rtsp_transport)", result);
        }
        result = avformat_write_header(context_, &options);
        if (result >= 0) {
            header_written_ = true;
        }
        const bool has_unrecognized_options = result >= 0 && av_dict_count(options) != 0;
        av_dict_free(&options);
        if (result < 0) {
            throw_ffmpeg_error("avformat_write_header", result);
        }
        if (has_unrecognized_options) {
            throw std::runtime_error(
                "FFmpeg RTSP muxer did not recognize one or more output options");
        }
    } catch (...) {
        stop();
        throw;
    }
}

void RtspOutput::write(const MediaPacket& packet) {
    if (context_ == nullptr || !header_written_) {
        throw std::logic_error("RTSP output is not started");
    }
    if (owner_thread_ != std::this_thread::get_id()) {
        throw std::logic_error("RTSP context must only be used by its output worker thread");
    }
    const auto stream_index = stream_indexes_.find(packet.stream_id());
    if (stream_index == stream_indexes_.end()) {
        throw std::invalid_argument("packet belongs to an unbound RTSP stream");
    }
    if (cancelled_.load(std::memory_order_acquire)) {
        throw std::runtime_error("RTSP write cancelled");
    }
    const auto& payload = *packet.payload();
    if (payload.empty() || payload.size() > static_cast<std::size_t>(INT_MAX)) {
        throw std::invalid_argument("RTSP packet payload size is invalid");
    }
    if (packet.pts_ns() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        packet.dts_ns() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        packet.duration_ns() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw std::invalid_argument("RTSP packet timestamp exceeds signed FFmpeg range");
    }

    AVPacket* av_packet = av_packet_alloc();
    if (av_packet == nullptr) {
        throw std::bad_alloc();
    }
    int result = av_new_packet(av_packet, static_cast<int>(payload.size()));
    if (result < 0) {
        av_packet_free(&av_packet);
        throw_ffmpeg_error("av_new_packet", result);
    }
    std::copy(payload.begin(), payload.end(), av_packet->data);
    av_packet->stream_index = stream_index->second;
    av_packet->pts = static_cast<std::int64_t>(packet.pts_ns());
    av_packet->dts = static_cast<std::int64_t>(packet.dts_ns());
    av_packet->duration = static_cast<std::int64_t>(packet.duration_ns());
    if (packet.keyframe()) {
        av_packet->flags |= AV_PKT_FLAG_KEY;
    }
    av_packet_rescale_ts(av_packet, AVRational{1, 1000000000},
                         context_->streams[stream_index->second]->time_base);
    set_operation_deadline();
    result = av_interleaved_write_frame(context_, av_packet);
    av_packet_free(&av_packet);
    if (result < 0) {
        throw_ffmpeg_error("av_interleaved_write_frame", result);
    }
}

void RtspOutput::request_stop() noexcept {
    cancelled_.store(true, std::memory_order_release);
    deadline_ns_.store(monotonic_now_ns(), std::memory_order_release);
}

void RtspOutput::stop() noexcept {
    if (context_ == nullptr) {
        stream_indexes_.clear();
        header_written_ = false;
        owner_thread_ = std::thread::id{};
        cancelled_.store(false, std::memory_order_release);
        return;
    }
    if (header_written_) {
        cancelled_.store(false, std::memory_order_release);
        set_operation_deadline();
        av_write_trailer(context_);
    }
    release_context();
}

std::string RtspOutput::name() const { return "RTSP"; }

int RtspOutput::interrupt_callback(void* opaque) noexcept {
    auto* output = static_cast<RtspOutput*>(opaque);
    if (output == nullptr) {
        return 1;
    }
    if (output->cancelled_.load(std::memory_order_acquire)) {
        return 1;
    }
    const std::int64_t deadline = output->deadline_ns_.load(std::memory_order_acquire);
    if (deadline != 0 && monotonic_now_ns() >= deadline) {
        return 1;
    }
    return 0;
}

void RtspOutput::set_operation_deadline() noexcept {
    const auto timeout = std::chrono::duration_cast<std::chrono::nanoseconds>(io_timeout_);
    deadline_ns_.store(monotonic_now_ns() + timeout.count(), std::memory_order_release);
}

void RtspOutput::release_context() noexcept {
    if (context_ != nullptr) {
        if (context_->pb != nullptr && context_->oformat != nullptr &&
            (context_->oformat->flags & AVFMT_NOFILE) == 0) {
            avio_closep(&context_->pb);
        }
        avformat_free_context(context_);
        context_ = nullptr;
    }
    stream_indexes_.clear();
    owner_thread_ = std::thread::id{};
    header_written_ = false;
    cancelled_.store(false, std::memory_order_release);
}

}  // namespace vtb
