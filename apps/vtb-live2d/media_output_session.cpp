#include "media_output_session.hpp"

#include <chrono>
#include <exception>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "vtb/instrumentation/metrics_jsonl_reporter.hpp"
#include "vtb/instrumentation/timer.hpp"
#include "vtb/output/output_manager.hpp"

namespace vtb {
namespace {
class TimedOutput final : public Output {
public:
    TimedOutput(
        std::unique_ptr<Output> output,
        instrumentation::TimerHandle write_timer)
        : output_(std::move(output)), write_timer_(std::move(write_timer)) {}
    ~TimedOutput() override = default;

    TimedOutput(const TimedOutput&) = delete;
    TimedOutput& operator=(const TimedOutput&) = delete;
    TimedOutput(TimedOutput&&) = delete;
    TimedOutput& operator=(TimedOutput&&) = delete;

    bool supports(const StreamDescriptor& descriptor) const override {
        return output_->supports(descriptor);
    }

    void start(const std::vector<StreamDescriptor>& descriptors) override {
        output_->start(descriptors);
    }

    void write(MediaPacket packet) override {
        auto timer = write_timer_.scope();
        output_->write(std::move(packet));
    }

    void request_stop() noexcept override { output_->request_stop(); }
    void stop() noexcept override { output_->stop(); }
    std::string name() const override { return output_->name(); }

private:
    std::unique_ptr<Output> output_;
    instrumentation::TimerHandle write_timer_;
};

}  // namespace

class MediaOutputSession::Impl final {
public:
    Impl(
        std::unique_ptr<Output> rtsp_output,
        std::unique_ptr<Output> uvc_output,
        MediaOutputSessionConfig config)
        : config_(std::move(config)),
          rtsp_write_timer_(timers_.register_timer("live2d.rtsp_write")),
          uvc_write_timer_(timers_.register_timer("live2d.uvc_write")) {
        initialize(
            std::move(rtsp_output),
            std::move(uvc_output));
    }

    void initialize(
        std::unique_ptr<Output> rtsp_output,
        std::unique_ptr<Output> uvc_output) {
        if (!rtsp_output) {
            throw std::invalid_argument("RTSP media output is required");
        }
        if (config_.rtsp_output_id.empty() || config_.uvc_output_id.empty() ||
            config_.h264_stream_id.empty() || config_.mjpeg_stream_id.empty() ||
            config_.metrics_source.empty()) {
            throw std::invalid_argument(
                "Media output identifiers must not be empty");
        }
        OutputOptions options;
        options.queue_capacity = 16;
        manager_.add(
            config_.rtsp_output_id,
            std::make_unique<TimedOutput>(
                std::move(rtsp_output), rtsp_write_timer_),
            options);
        if (uvc_output) {
            manager_.add(
                config_.uvc_output_id,
                std::make_unique<TimedOutput>(
                    std::move(uvc_output), uvc_write_timer_),
                options);
            uvc_enabled_ = true;
        }
        if (!config_.metrics_path.empty()) {
            metrics_reporter_ =
                std::make_unique<instrumentation::MetricsJsonlReporter>(
                    timers_,
                    config_.metrics_source,
                    config_.metrics_path,
                    config_.metrics_report_interval);
            metrics_reporter_->start();
        }
    }
    ~Impl() {
        // The owning application has already joined the producer before this point.
        try {
            finish();
        } catch (...) {
        }
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    EncodedVideoSink sink() noexcept {
        EncodedVideoSink result;
        result.context = this;
        result.configure = configure_callback;
        result.publish = publish_callback;
        return result;
    }

    void rethrow_if_failed() const {
        std::exception_ptr error;
        {
            std::lock_guard<std::mutex> lock(error_mutex_);
            error = callback_error_;
        }
        if (error) {
            std::rethrow_exception(error);
        }
    }

    void finish() {
        if (started_output_ids_.empty()) {
            finish_metrics();
            rethrow_if_failed();
            return;
        }
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline) {
            bool drained = true;
            for (const OutputStatus& status : manager_.status()) {
                if (status.queue_depth != 0 ||
                    status.written + status.dropped < status.published) {
                    drained = false;
                    break;
                }
            }
            if (drained) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        for (const std::string& output_id : started_output_ids_) {
            manager_.stop(output_id);
        }
        const std::vector<OutputStatus> statuses = manager_.status();
        for (const OutputStatus& status : statuses) {
            std::cout << "VTB_OUTPUT_RESULT output_id=" << status.id
                      << " published=" << status.published
                      << " written=" << status.written
                      << " dropped=" << status.dropped
                      << " errors=" << status.errors
                      << " reconnects=" << status.reconnects
                      << " queue_depth=" << status.queue_depth
                      << " queue_capacity=" << status.queue_capacity << "\n";
        }
        started_output_ids_.clear();
        finish_metrics();
        rethrow_if_failed();
        for (const OutputStatus& status : statuses) {
            if (!status.streams.empty() && status.written == 0) {
                throw std::runtime_error(
                    "Media output " + status.id +
                    " did not write any encoded packets: " + status.last_error);
            }
        }
    }

private:
    void finish_metrics() noexcept {
        if (!metrics_reporter_ || metrics_finished_) {
            return;
        }
        metrics_reporter_->stop();
        metrics_finished_ = true;
        std::cout << "VTB_METRICS_RESULT source=" << metrics_reporter_->source()
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

    static std::int32_t configure_callback(
        void* context, const EncodedVideoStreamFormat* format) noexcept {
        auto& self = *static_cast<Impl*>(context);
        try {
            self.configure(*format);
            return 0;
        } catch (...) {
            self.remember_failure();
            return -1;
        }
    }

    static std::int32_t publish_callback(
        void* context, const EncodedVideoPacketView* packet) noexcept {
        auto& self = *static_cast<Impl*>(context);
        try {
            self.publish(*packet);
            return 0;
        } catch (...) {
            self.remember_failure();
            return -1;
        }
    }

    void remember_failure() noexcept {
        std::lock_guard<std::mutex> lock(error_mutex_);
        if (!callback_error_) {
            callback_error_ = std::current_exception();
        }
    }

    void configure(const EncodedVideoStreamFormat& format) {
        if (format.codec_configuration_size >
            std::numeric_limits<std::size_t>::max()) {
            throw std::invalid_argument("Encoded stream configuration is too large");
        }
        if (format.codec == EncodedVideoCodec::h264) {
            configure_h264(format);
            return;
        }
        if (format.codec == EncodedVideoCodec::mjpeg) {
            configure_mjpeg(format);
            return;
        }
        throw std::invalid_argument("Unsupported encoded video codec");
    }

    void configure_h264(const EncodedVideoStreamFormat& format) {
        if (h264_configured_ || format.codec_configuration == nullptr ||
            format.codec_configuration_size == 0 ||
            format.codec_configuration_size > std::numeric_limits<std::size_t>::max()) {
            throw std::invalid_argument("Invalid or repeated H.264 stream configuration");
        }
        std::vector<std::uint8_t> configuration(
            format.codec_configuration,
            format.codec_configuration +
                static_cast<std::size_t>(format.codec_configuration_size));
        manager_.register_stream(StreamDescriptor::video(
            config_.h264_stream_id, 1, "h264", format.width,
            format.height, format.fps, 1, true, std::move(configuration)));
        manager_.bind(config_.rtsp_output_id, config_.h264_stream_id);
        start_output(config_.rtsp_output_id);
        h264_configured_ = true;
    }

    void configure_mjpeg(const EncodedVideoStreamFormat& format) {
        if (!uvc_enabled_ || mjpeg_configured_ ||
            format.codec_configuration != nullptr ||
            format.codec_configuration_size != 0) {
            throw std::invalid_argument("Invalid or unavailable MJPEG stream configuration");
        }
        manager_.register_stream(StreamDescriptor::video(
            config_.mjpeg_stream_id, 1, "mjpeg", format.width,
            format.height, format.fps, 1, true, {}));
        manager_.bind(config_.uvc_output_id, config_.mjpeg_stream_id);
        start_output(config_.uvc_output_id);
        mjpeg_configured_ = true;
    }

    void start_output(const std::string& output_id) {
        manager_.start(output_id);
        started_output_ids_.push_back(output_id);
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            const OutputStatus status = status_for(output_id);
            if (status.state == OutputState::running) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        throw std::runtime_error(
            "Media output did not start: " + status_for(output_id).last_error);
    }

    OutputStatus status_for(const std::string& output_id) const {
        for (const OutputStatus& status : manager_.status()) {
            if (status.id == output_id) {
                return status;
            }
        }
        throw std::logic_error("Media output status is missing: " + output_id);
    }

    void publish(const EncodedVideoPacketView& view) {
        std::string stream_id;
        if (view.codec == EncodedVideoCodec::h264 && h264_configured_) {
            stream_id = config_.h264_stream_id;
        } else if (view.codec == EncodedVideoCodec::mjpeg && mjpeg_configured_) {
            stream_id = config_.mjpeg_stream_id;
        } else {
            throw std::invalid_argument("Encoded packet codec is not configured");
        }
        if (view.data == nullptr || view.size == 0 ||
            view.size > std::numeric_limits<std::size_t>::max()) {
            throw std::invalid_argument("Invalid encoded video packet");
        }
        // The producer owns view.data only until this callback returns.
        auto payload = std::make_shared<const std::vector<std::uint8_t>>(
            view.data, view.data + static_cast<std::size_t>(view.size));
        MediaPacket packet(std::move(stream_id), std::move(payload), view.pts_ns, view.dts_ns,
            view.duration_ns, view.sequence, view.keyframe != 0, false);
        // Zero enqueues is allowed during reconnect/keyframe wait/backpressure.
        manager_.publish(std::move(packet));
    }

    MediaOutputSessionConfig config_;
    instrumentation::TimerRegistry timers_;
    instrumentation::TimerHandle rtsp_write_timer_;
    instrumentation::TimerHandle uvc_write_timer_;
    std::unique_ptr<instrumentation::MetricsJsonlReporter> metrics_reporter_;
    OutputManager manager_;
    mutable std::mutex error_mutex_;
    std::exception_ptr callback_error_;
    std::vector<std::string> started_output_ids_;
    bool uvc_enabled_ = false;
    bool h264_configured_ = false;
    bool mjpeg_configured_ = false;
    bool metrics_finished_ = false;
};

MediaOutputSession::MediaOutputSession(
    std::unique_ptr<Output> rtsp_output,
    std::unique_ptr<Output> uvc_output,
    MediaOutputSessionConfig config)
    : impl_(std::make_unique<Impl>(
          std::move(rtsp_output),
          std::move(uvc_output),
          std::move(config))) {}
MediaOutputSession::~MediaOutputSession() = default;
EncodedVideoSink MediaOutputSession::sink() noexcept { return impl_->sink(); }
void MediaOutputSession::rethrow_if_failed() const { impl_->rethrow_if_failed(); }
void MediaOutputSession::finish() { impl_->finish(); }

}  // namespace vtb
