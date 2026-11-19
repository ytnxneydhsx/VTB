#include "rtsp_receiver.hh"

#include "av_frame_image.hh"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}

#include <chrono>
#include <memory>
#include <utility>

namespace vtb::desktop {
namespace {

    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;

    class ReadDeadline {
    public:
        const std::atomic<bool>& cancelled;
        Clock::time_point until;
    };

    int interrupt(void* data) {
        const auto& deadline = *static_cast<ReadDeadline*>(data);
        return deadline.cancelled.load() || Clock::now() > deadline.until;
    }

} // namespace

RtspReceiver::~RtspReceiver() { stop(); }

auto RtspReceiver::start(const QString& url) -> void {
    stop();
    cancelled_.store(false);
    worker_ = std::thread([this, url = url.toUtf8()] { receive(url); });
}

auto RtspReceiver::stop() -> void {
    cancelled_.store(true);
    if (worker_.joinable()) {
        worker_.join();
    }
    const std::scoped_lock lock(mutex_);
    latest_ = {};
    error_.clear();
}

auto RtspReceiver::takeFrame() -> QImage {
    const std::scoped_lock lock(mutex_);
    return std::exchange(latest_, {});
}

auto RtspReceiver::error() -> QString {
    const std::scoped_lock lock(mutex_);
    return error_;
}

auto RtspReceiver::fail(int result) -> void {
    if (cancelled_.load()) {
        return;
    }
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(result, message, sizeof(message));
    const std::scoped_lock lock(mutex_);
    error_ = QString::fromUtf8(message);
}

auto RtspReceiver::receive(const QByteArray& url) -> void {
    ReadDeadline deadline{ cancelled_, Clock::now() + 8s };
    auto* input = avformat_alloc_context();
    if (!input) {
        fail(AVERROR(ENOMEM));
        return;
    }
    input->interrupt_callback = { interrupt, &deadline };
    AVDictionary* options = nullptr;
    av_dict_set(&options, "rtsp_transport", "tcp", 0);
    auto result = avformat_open_input(&input, url.constData(), nullptr, &options);
    av_dict_free(&options);
    if (result < 0) {
        fail(result);
        return;
    }
    const auto close_input = [](AVFormatContext* ptr) { avformat_close_input(&ptr); };
    const std::unique_ptr<AVFormatContext, decltype(close_input)> format(input, close_input);
    result = avformat_find_stream_info(format.get(), nullptr);
    if (result < 0) {
        fail(result);
        return;
    }
    const AVCodec* decoder = nullptr;
    const auto video = av_find_best_stream(format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (video < 0) {
        fail(video);
        return;
    }
    const auto close_decoder = [](AVCodecContext* ptr) { avcodec_free_context(&ptr); };
    const std::unique_ptr<AVCodecContext, decltype(close_decoder)> codec(
        avcodec_alloc_context3(decoder), close_decoder);
    if (!codec) {
        fail(AVERROR(ENOMEM));
        return;
    }
    result = avcodec_parameters_to_context(codec.get(), format->streams[video]->codecpar);
    if (result >= 0) {
        codec->thread_count = 2;
        codec->thread_type = FF_THREAD_SLICE;
        result = avcodec_open2(codec.get(), decoder, nullptr);
    }
    if (result < 0) {
        fail(result);
        return;
    }
    const auto free_packet = [](AVPacket* ptr) { av_packet_free(&ptr); };
    const auto free_frame = [](AVFrame* ptr) { av_frame_free(&ptr); };
    const std::unique_ptr<AVPacket, decltype(free_packet)> packet(av_packet_alloc(), free_packet);
    const std::unique_ptr<AVFrame, decltype(free_frame)> frame(av_frame_alloc(), free_frame);
    AvFrameImage converter;
    if (!packet || !frame) {
        fail(AVERROR(ENOMEM));
        return;
    }
    while (!cancelled_.load()) {
        deadline.until = Clock::now() + 5s;
        result = av_read_frame(format.get(), packet.get());
        if (result < 0) {
            fail(result);
            return;
        }
        if (packet->stream_index != video) {
            av_packet_unref(packet.get());
            continue;
        }
        result = avcodec_send_packet(codec.get(), packet.get());
        av_packet_unref(packet.get());
        if (result < 0) {
            fail(result);
            return;
        }
        while (!cancelled_.load()) {
            result = avcodec_receive_frame(codec.get(), frame.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                break;
            }
            if (result < 0) {
                fail(result);
                return;
            }
            auto image = converter.convert(*frame);
            if (image.isNull()) {
                fail(AVERROR_INVALIDDATA);
                return;
            }
            const std::scoped_lock lock(mutex_);
            latest_ = std::move(image);
        }
    }
}

} // namespace vtb::desktop
