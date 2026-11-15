#include "media_encode_session.hpp"

#include <exception>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "mpp_encoder_session.hpp"
#include "vtb/instrumentation/metrics_jsonl_reporter.hpp"
#include "vtb/instrumentation/timer.hpp"

namespace vtb {

class MediaEncodeSession::Impl final {
public:
    Impl(
        EncodedVideoSink output_sink,
        MediaEncodeSessionConfig config)
        : output_sink_(output_sink),
          config_(std::move(config)),
          h264_timer_(timers_.register_timer("live2d.mpp_h264")),
          mjpeg_timer_(timers_.register_timer("live2d.mpp_mjpeg")),
          publish_timer_(timers_.register_timer("live2d.encoded_publish")),
          total_timer_(timers_.register_timer("live2d.encode_total")) {
        if (output_sink_.context == nullptr ||
            output_sink_.configure == nullptr ||
            output_sink_.publish == nullptr) {
            throw std::invalid_argument(
                "MediaEncodeSession requires a complete encoded video sink");
        }
        if (config_.metrics_report_interval.count() <= 0) {
            throw std::invalid_argument(
                "Media encode metrics interval must be positive");
        }
        if (!config_.metrics_path.empty()) {
            metrics_reporter_ =
                std::make_unique<instrumentation::MetricsJsonlReporter>(
                    timers_,
                    "vtb-live2d.encode",
                    config_.metrics_path,
                    config_.metrics_report_interval);
            metrics_reporter_->start();
        }
    }

    ~Impl() {
        try {
            finish();
        } catch (...) {
        }
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    Nv12FrameSink sink() noexcept {
        Nv12FrameSink result;
        result.context = this;
        result.submit = submit_callback;
        return result;
    }

    void rethrow_if_failed() const {
        std::exception_ptr failure;
        {
            std::lock_guard<std::mutex> lock(failure_mutex_);
            failure = failure_;
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    void finish() {
        if (finished_) {
            rethrow_if_failed();
            return;
        }
        finished_ = true;
        h264_encoder_.shutdown();
        mjpeg_encoder_.shutdown();
        finish_metrics();
        std::cout << "VTB_MEDIA_ENCODE_RESULT"
                  << " submitted=" << submitted_frames_
                  << " h264=" << h264_frames_
                  << " mjpeg=" << mjpeg_frames_
                  << " h264_bytes=" << h264_bytes_
                  << " mjpeg_bytes=" << mjpeg_bytes_ << "\n";
        rethrow_if_failed();
        if (initialized_ && h264_frames_ != submitted_frames_) {
            throw std::runtime_error(
                "Media encoder H.264 frame count does not match NV12 input");
        }
    }

private:
    static std::int32_t submit_callback(
        void* context,
        const Nv12FrameView* frame) noexcept {
        auto& self = *static_cast<Impl*>(context);
        try {
            if (frame == nullptr) {
                throw std::invalid_argument("NV12 frame view is null");
            }
            self.submit(*frame);
            return 0;
        } catch (...) {
            self.remember_failure(std::current_exception());
            return -1;
        }
    }

    void remember_failure(std::exception_ptr failure) noexcept {
        std::lock_guard<std::mutex> lock(failure_mutex_);
        if (!failure_) {
            failure_ = std::move(failure);
        }
    }

    void initialize(const Nv12FrameView& frame) {
        validate_frame(frame);
        if (config_.uvc_fps > frame.frames_per_second) {
            throw std::invalid_argument(
                "UVC frame rate exceeds the NV12 render frame rate");
        }
        if (frame.dma_buf_size >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())) {
            throw std::invalid_argument("NV12 DMA-BUF is too large");
        }
        const std::size_t input_size =
            static_cast<std::size_t>(frame.dma_buf_size);
        h264_encoder_.initialize(
            MppVideoCodec::h264,
            frame.dma_buf_fd,
            input_size,
            frame.width,
            frame.height,
            frame.horizontal_stride,
            frame.vertical_stride,
            frame.frames_per_second);
        const std::vector<std::uint8_t> codec_configuration =
            h264_encoder_.h264_codec_configuration();
        EncodedVideoStreamFormat h264_format;
        h264_format.codec = EncodedVideoCodec::h264;
        h264_format.width = frame.width;
        h264_format.height = frame.height;
        h264_format.fps = frame.frames_per_second;
        h264_format.codec_configuration = codec_configuration.data();
        h264_format.codec_configuration_size = codec_configuration.size();
        if (output_sink_.configure(output_sink_.context, &h264_format) != 0) {
            throw std::runtime_error(
                "Encoded video sink rejected H.264 configuration");
        }

        if (config_.uvc_fps > 0) {
            mjpeg_encoder_.initialize(
                MppVideoCodec::mjpeg,
                frame.dma_buf_fd,
                input_size,
                frame.width,
                frame.height,
                frame.horizontal_stride,
                frame.vertical_stride,
                config_.uvc_fps);
            EncodedVideoStreamFormat mjpeg_format;
            mjpeg_format.codec = EncodedVideoCodec::mjpeg;
            mjpeg_format.width = frame.width;
            mjpeg_format.height = frame.height;
            mjpeg_format.fps = config_.uvc_fps;
            if (output_sink_.configure(
                    output_sink_.context, &mjpeg_format) != 0) {
                throw std::runtime_error(
                    "Encoded video sink rejected MJPEG configuration");
            }
        }

        input_fd_ = frame.dma_buf_fd;
        input_size_ = frame.dma_buf_size;
        buffer_id_ = frame.buffer_id;
        stream_generation_ = frame.stream_generation;
        width_ = frame.width;
        height_ = frame.height;
        horizontal_stride_ = frame.horizontal_stride;
        vertical_stride_ = frame.vertical_stride;
        render_fps_ = frame.frames_per_second;
        initialized_ = true;
    }

    void validate_frame(const Nv12FrameView& frame) const {
        if (frame.dma_buf_fd < 0 || frame.dma_buf_size == 0 ||
            frame.stream_generation == 0 || frame.width == 0 ||
            frame.height == 0 || frame.horizontal_stride < frame.width ||
            frame.vertical_stride < frame.height ||
            frame.frames_per_second == 0 || frame.timestamp_ns == 0 ||
            frame.sequence == 0) {
            throw std::invalid_argument("NV12 frame view is invalid");
        }
        if (frame.end_of_stream != 0 && frame.end_of_stream != 1) {
            throw std::invalid_argument(
                "NV12 end-of-stream flag must be zero or one");
        }
    }

    void require_same_stream(const Nv12FrameView& frame) const {
        validate_frame(frame);
        if (frame.dma_buf_fd != input_fd_ ||
            frame.dma_buf_size != input_size_ ||
            frame.buffer_id != buffer_id_ ||
            frame.stream_generation != stream_generation_ ||
            frame.width != width_ || frame.height != height_ ||
            frame.horizontal_stride != horizontal_stride_ ||
            frame.vertical_stride != vertical_stride_ ||
            frame.frames_per_second != render_fps_) {
            throw std::invalid_argument(
                "NV12 stream contract changed without encoder reconfiguration");
        }
        if (frame.sequence != submitted_frames_ + 1) {
            throw std::invalid_argument(
                "NV12 frame sequence is not contiguous");
        }
    }

    void submit(const Nv12FrameView& frame) {
        if (finished_) {
            throw std::logic_error("MediaEncodeSession is already finished");
        }
        if (end_of_stream_received_) {
            throw std::logic_error(
                "MediaEncodeSession received a frame after end-of-stream");
        }
        if (!initialized_) {
            initialize(frame);
        }
        require_same_stream(frame);

        auto total_scope = total_timer_.scope();
        MppEncodedPacket h264_packet;
        {
            auto h264_scope = h264_timer_.scope();
            h264_packet = h264_encoder_.encode(
                frame.timestamp_ns, frame.end_of_stream != 0);
        }
        publish_packet(
            EncodedVideoCodec::h264,
            h264_packet,
            1000000000ULL / frame.frames_per_second,
            frame.sequence);
        ++h264_frames_;
        h264_bytes_ += h264_packet.size();

        if (should_encode_mjpeg(frame.sequence)) {
            MppEncodedPacket mjpeg_packet;
            {
                auto mjpeg_scope = mjpeg_timer_.scope();
                mjpeg_packet =
                    mjpeg_encoder_.encode(frame.timestamp_ns, false);
            }
            publish_packet(
                EncodedVideoCodec::mjpeg,
                mjpeg_packet,
                1000000000ULL / config_.uvc_fps,
                mjpeg_frames_ + 1);
            ++mjpeg_frames_;
            mjpeg_bytes_ += mjpeg_packet.size();
        }
        ++submitted_frames_;
        if (frame.end_of_stream != 0) {
            end_of_stream_received_ = true;
        }
    }

    bool should_encode_mjpeg(std::uint64_t sequence) const noexcept {
        if (config_.uvc_fps == 0) {
            return false;
        }
        if (sequence == 1) {
            return true;
        }
        const std::uint64_t frame_index = sequence - 1;
        const std::uint64_t current =
            frame_index * config_.uvc_fps / render_fps_;
        const std::uint64_t previous =
            (frame_index - 1) * config_.uvc_fps / render_fps_;
        return current > previous;
    }

    void publish_packet(
        EncodedVideoCodec codec,
        const MppEncodedPacket& encoded,
        std::uint64_t duration_ns,
        std::uint64_t sequence) {
        EncodedVideoPacketView packet;
        packet.codec = codec;
        packet.data = encoded.data();
        packet.size = encoded.size();
        packet.pts_ns = encoded.pts_ns();
        packet.dts_ns = encoded.dts_ns();
        packet.duration_ns = duration_ns;
        packet.sequence = sequence;
        if (encoded.keyframe()) {
            packet.keyframe = 1;
        }
        auto publish_scope = publish_timer_.scope();
        if (output_sink_.publish(output_sink_.context, &packet) != 0) {
            throw std::runtime_error(
                "Encoded video sink rejected an MPP packet");
        }
    }

    void finish_metrics() noexcept {
        if (!metrics_reporter_ || metrics_finished_) {
            return;
        }
        metrics_reporter_->stop();
        metrics_finished_ = true;
        std::cout << "VTB_METRICS_RESULT source="
                  << metrics_reporter_->source()
                  << " path=" << metrics_reporter_->output_path()
                  << " records=" << metrics_reporter_->record_count()
                  << " failed=";
        if (metrics_reporter_->failed()) {
            std::cout << "1 error=" << metrics_reporter_->last_error();
        } else {
            std::cout << "0";
        }
        std::cout << "\n";
    }

    EncodedVideoSink output_sink_;
    MediaEncodeSessionConfig config_;
    instrumentation::TimerRegistry timers_;
    instrumentation::TimerHandle h264_timer_;
    instrumentation::TimerHandle mjpeg_timer_;
    instrumentation::TimerHandle publish_timer_;
    instrumentation::TimerHandle total_timer_;
    std::unique_ptr<instrumentation::MetricsJsonlReporter> metrics_reporter_;
    MppEncoderSession h264_encoder_;
    MppEncoderSession mjpeg_encoder_;
    mutable std::mutex failure_mutex_;
    std::exception_ptr failure_;
    int input_fd_ = -1;
    std::uint64_t input_size_ = 0;
    std::uint64_t buffer_id_ = 0;
    std::uint64_t stream_generation_ = 0;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t horizontal_stride_ = 0;
    std::uint32_t vertical_stride_ = 0;
    std::uint32_t render_fps_ = 0;
    std::uint64_t submitted_frames_ = 0;
    std::uint64_t h264_frames_ = 0;
    std::uint64_t mjpeg_frames_ = 0;
    std::uint64_t h264_bytes_ = 0;
    std::uint64_t mjpeg_bytes_ = 0;
    bool initialized_ = false;
    bool end_of_stream_received_ = false;
    bool finished_ = false;
    bool metrics_finished_ = false;
};

MediaEncodeSession::MediaEncodeSession(
    EncodedVideoSink output_sink,
    MediaEncodeSessionConfig config)
    : impl_(std::make_unique<Impl>(output_sink, std::move(config))) {}

MediaEncodeSession::~MediaEncodeSession() = default;

Nv12FrameSink MediaEncodeSession::sink() noexcept {
    return impl_->sink();
}

void MediaEncodeSession::rethrow_if_failed() const {
    impl_->rethrow_if_failed();
}

void MediaEncodeSession::finish() {
    impl_->finish();
}

}  // namespace vtb
