#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

#include "vtb/output/output_manager.hpp"
#include "vtb/output/rtsp_output.hpp"

namespace {

volatile std::sig_atomic_t stop_requested = 0;

void handle_signal(int) { stop_requested = 1; }

class PublisherOptions {
public:
    std::string input_format = "mpegts";
    std::string input_url = "pipe:0";
    std::string rtsp_url;
    std::string stream_id = "face3d-video";
    std::size_t queue_capacity = 16;
};

class InputContextDeleter {
public:
    void operator()(AVFormatContext* context) const noexcept {
        if (context != nullptr) {
            avformat_close_input(&context);
        }
    }
};

class PacketDeleter {
public:
    void operator()(AVPacket* packet) const noexcept {
        if (packet != nullptr) {
            av_packet_free(&packet);
        }
    }
};

using InputContext = std::unique_ptr<AVFormatContext, InputContextDeleter>;
using Packet = std::unique_ptr<AVPacket, PacketDeleter>;

std::string ffmpeg_error(int error) {
    std::vector<char> buffer(AV_ERROR_MAX_STRING_SIZE);
    av_strerror(error, buffer.data(), buffer.size());
    return std::string(buffer.data());
}

[[noreturn]] void throw_ffmpeg_error(const std::string& operation, int error) {
    throw std::runtime_error(operation + ": " + ffmpeg_error(error));
}

std::size_t parse_size(const std::string& text, const std::string& field) {
    std::size_t value = 0;
    const char* first = text.data();
    const char* last = first + text.size();
    const auto parsed = std::from_chars(first, last, value, 10);
    if (parsed.ec != std::errc{} || parsed.ptr != last || value == 0) {
        throw std::invalid_argument(field + " must be a positive integer");
    }
    return value;
}

PublisherOptions parse_arguments(int argc, char** argv) {
    PublisherOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help") {
            std::cout
                << "Usage: vtb-media-output-publisher --rtsp-url URL [options]\n"
                << "  --input-format FORMAT  FFmpeg demuxer name (default: mpegts)\n"
                << "  --input URL            FFmpeg input URL (default: pipe:0)\n"
                << "  --stream-id ID         OutputManager stream ID\n"
                << "  --queue N              Output queue capacity (default: 16)\n";
            std::exit(0);
        }
        if (index + 1 >= argc) {
            throw std::invalid_argument("missing value after " + argument);
        }
        const std::string value = argv[++index];
        if (argument == "--input-format") {
            options.input_format = value;
        } else if (argument == "--input") {
            options.input_url = value;
        } else if (argument == "--rtsp-url") {
            options.rtsp_url = value;
        } else if (argument == "--stream-id") {
            options.stream_id = value;
        } else if (argument == "--queue") {
            options.queue_capacity = parse_size(value, "queue capacity");
        } else {
            throw std::invalid_argument("unknown argument: " + argument);
        }
    }
    if (options.rtsp_url.empty()) {
        throw std::invalid_argument("--rtsp-url is required");
    }
    if (options.input_format.empty() || options.input_url.empty() ||
        options.stream_id.empty()) {
        throw std::invalid_argument("input format, input URL and stream ID must not be empty");
    }
    return options;
}

InputContext open_input(const PublisherOptions& options) {
    const AVInputFormat* input_format = av_find_input_format(options.input_format.c_str());
    if (input_format == nullptr) {
        throw std::invalid_argument("unknown FFmpeg input format: " + options.input_format);
    }

    AVDictionary* input_options = nullptr;
    av_dict_set(&input_options, "probesize", "32768", 0);
    av_dict_set(&input_options, "analyzeduration", "1000000", 0);
    AVFormatContext* raw_context = nullptr;
    const int open_result = avformat_open_input(
        &raw_context, options.input_url.c_str(), input_format, &input_options);
    av_dict_free(&input_options);
    if (open_result < 0) {
        throw_ffmpeg_error("avformat_open_input", open_result);
    }

    InputContext context(raw_context);
    const int stream_info_result = avformat_find_stream_info(context.get(), nullptr);
    if (stream_info_result < 0) {
        throw_ffmpeg_error("avformat_find_stream_info", stream_info_result);
    }
    return context;
}

std::vector<std::uint8_t> codec_configuration(const AVCodecParameters& parameters) {
    if (parameters.extradata == nullptr || parameters.extradata_size <= 0) {
        throw std::runtime_error(
            "H.264 input did not expose SPS/PPS codec configuration");
    }
    return std::vector<std::uint8_t>(
        parameters.extradata,
        parameters.extradata + parameters.extradata_size);
}

AVRational choose_frame_rate(AVFormatContext* context, AVStream* stream) {
    AVRational frame_rate = av_guess_frame_rate(context, stream, nullptr);
    if (frame_rate.num <= 0 || frame_rate.den <= 0) {
        frame_rate = stream->avg_frame_rate;
    }
    if (frame_rate.num <= 0 || frame_rate.den <= 0) {
        frame_rate = AVRational{30, 1};
    }
    return frame_rate;
}

std::uint64_t steady_now_ns() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

std::uint64_t timestamp_offset_ns(std::int64_t timestamp,
                                  std::int64_t origin,
                                  AVRational time_base) {
    if (timestamp == AV_NOPTS_VALUE || timestamp <= origin) {
        return 0;
    }
    const std::int64_t offset = timestamp - origin;
    const std::int64_t value = av_rescale_q(
        offset, time_base, AVRational{1, 1000000000});
    if (value <= 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(value);
}

std::uint64_t packet_duration_ns(const AVPacket& packet,
                                 AVRational time_base,
                                 AVRational frame_rate) {
    if (packet.duration > 0) {
        const std::int64_t duration = av_rescale_q(
            packet.duration, time_base, AVRational{1, 1000000000});
        if (duration > 0) {
            return static_cast<std::uint64_t>(duration);
        }
    }
    const std::int64_t duration = av_rescale_q(
        frame_rate.den, AVRational{1, frame_rate.num},
        AVRational{1, 1000000000});
    if (duration <= 0) {
        throw std::runtime_error("unable to derive a positive video packet duration");
    }
    return static_cast<std::uint64_t>(duration);
}

vtb::OutputStatus output_status(vtb::OutputManager& manager) {
    const std::vector<vtb::OutputStatus> statuses = manager.status();
    if (statuses.size() != 1) {
        throw std::logic_error("publisher expected exactly one output");
    }
    return statuses.front();
}

void wait_until_running(vtb::OutputManager& manager) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        const vtb::OutputStatus status = output_status(manager);
        if (status.state == vtb::OutputState::running) {
            return;
        }
        if (status.state == vtb::OutputState::failed) {
            throw std::runtime_error("RTSP output failed: " + status.last_error);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const vtb::OutputStatus status = output_status(manager);
    throw std::runtime_error("RTSP output did not start: " + status.last_error);
}

void print_status(const vtb::OutputStatus& status, const std::string& prefix) {
    std::cerr << prefix
              << " published=" << status.published
              << " written=" << status.written
              << " dropped=" << status.dropped
              << " errors=" << status.errors
              << " queue=" << status.queue_depth << '/' << status.queue_capacity;
    if (!status.last_error.empty()) {
        std::cerr << " last_error=\"" << status.last_error << '"';
    }
    std::cerr << '\n';
}

void drain_output(vtb::OutputManager& manager) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        const vtb::OutputStatus status = output_status(manager);
        if (status.queue_depth == 0 &&
            status.written + status.dropped >= status.published) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

int publish(const PublisherOptions& options) {
    InputContext input = open_input(options);
    const int video_index = av_find_best_stream(
        input.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_index < 0) {
        throw_ffmpeg_error("av_find_best_stream(video)", video_index);
    }

    AVStream* stream = input->streams[video_index];
    const AVCodecParameters& parameters = *stream->codecpar;
    if (parameters.codec_id != AV_CODEC_ID_H264) {
        throw std::invalid_argument("publisher currently accepts H.264 video only");
    }
    if (parameters.width <= 0 || parameters.height <= 0) {
        throw std::runtime_error("input video dimensions are missing");
    }
    const AVRational frame_rate = choose_frame_rate(input.get(), stream);

    const vtb::StreamDescriptor descriptor = vtb::StreamDescriptor::video(
        options.stream_id, 1, "h264",
        static_cast<std::uint32_t>(parameters.width),
        static_cast<std::uint32_t>(parameters.height),
        static_cast<std::uint32_t>(frame_rate.num),
        static_cast<std::uint32_t>(frame_rate.den), true,
        codec_configuration(parameters));

    vtb::OutputManager manager;
    manager.register_stream(descriptor);
    vtb::OutputOptions output_options;
    output_options.queue_capacity = options.queue_capacity;
    manager.add("rtsp", std::make_unique<vtb::RtspOutput>(options.rtsp_url),
                output_options);
    manager.bind("rtsp", descriptor.id());
    manager.start("rtsp");
    wait_until_running(manager);

    std::cerr << "RTSP output running via OutputManager: " << options.rtsp_url
              << " stream=" << parameters.width << 'x' << parameters.height
              << ' ' << frame_rate.num << '/' << frame_rate.den << " fps\n";

    Packet packet(av_packet_alloc());
    if (!packet) {
        throw std::bad_alloc();
    }

    bool have_origin = false;
    std::int64_t timestamp_origin = 0;
    const std::uint64_t monotonic_origin = steady_now_ns();
    std::uint64_t sequence = 0;
    while (stop_requested == 0) {
        const int read_result = av_read_frame(input.get(), packet.get());
        if (read_result == AVERROR_EOF) {
            break;
        }
        if (read_result == AVERROR(EAGAIN)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (read_result < 0) {
            throw_ffmpeg_error("av_read_frame", read_result);
        }
        if (packet->stream_index != video_index || packet->size <= 0 ||
            packet->data == nullptr) {
            av_packet_unref(packet.get());
            continue;
        }
        if ((packet->flags & AV_PKT_FLAG_CORRUPT) != 0) {
            av_packet_unref(packet.get());
            continue;
        }

        if (!have_origin) {
            if (packet->dts != AV_NOPTS_VALUE) {
                timestamp_origin = packet->dts;
            } else if (packet->pts != AV_NOPTS_VALUE) {
                timestamp_origin = packet->pts;
            }
            have_origin = true;
        }

        const std::uint64_t duration_ns = packet_duration_ns(
            *packet, stream->time_base, frame_rate);
        std::uint64_t pts_offset_ns = sequence * duration_ns;
        std::uint64_t dts_offset_ns = pts_offset_ns;
        if (packet->pts != AV_NOPTS_VALUE) {
            pts_offset_ns = timestamp_offset_ns(
                packet->pts, timestamp_origin, stream->time_base);
        }
        if (packet->dts != AV_NOPTS_VALUE) {
            dts_offset_ns = timestamp_offset_ns(
                packet->dts, timestamp_origin, stream->time_base);
        }

        const auto target_time = std::chrono::steady_clock::time_point(
            std::chrono::nanoseconds(monotonic_origin + dts_offset_ns));
        std::this_thread::sleep_until(target_time);

        auto payload = std::make_shared<const std::vector<std::uint8_t>>(
            packet->data, packet->data + packet->size);
        const bool keyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;
        const vtb::MediaPacket media_packet(
            descriptor.id(), std::move(payload),
            monotonic_origin + pts_offset_ns,
            monotonic_origin + dts_offset_ns,
            duration_ns, sequence, keyframe, false);
        manager.publish(media_packet);
        ++sequence;
        av_packet_unref(packet.get());

        if (sequence % 300 == 0) {
            print_status(output_status(manager), "OutputManager status:");
        }
    }

    drain_output(manager);
    const vtb::OutputStatus final_status = output_status(manager);
    print_status(final_status, "OutputManager final:");
    manager.stop("rtsp");
    if (final_status.errors != 0) {
        return 2;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    std::signal(SIGHUP, handle_signal);
    try {
        return publish(parse_arguments(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "vtb-media-output-publisher: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "vtb-media-output-publisher: unknown failure\n";
        return 1;
    }
}
