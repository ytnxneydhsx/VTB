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
        std::unique_ptr<Output> output,
        std::string metrics_path,
        std::chrono::milliseconds metrics_report_interval,
        std::string output_id,
        std::string stream_id,
        std::string metrics_source,
        std::string write_timer_name)
        : output_id_(std::move(output_id)),
          stream_id_(std::move(stream_id)),
          metrics_source_(std::move(metrics_source)),
          write_timer_(timers_.register_timer(std::move(write_timer_name))) {
        initialize(
            std::move(output),
            std::move(metrics_path),
            metrics_report_interval);
    }

    void initialize(
        std::unique_ptr<Output> output,
        std::string metrics_path,
        std::chrono::milliseconds metrics_report_interval) {
        if (!output) {
            throw std::invalid_argument("Media output is required");
        }
        if (output_id_.empty() || stream_id_.empty() ||
            metrics_source_.empty()) {
            throw std::invalid_argument(
                "Media output identifiers must not be empty");
        }
        OutputOptions options;
        options.queue_capacity = 16;
        manager_.add(
            output_id_,
            std::make_unique<TimedOutput>(std::move(output), write_timer_),
            options);
        if (!metrics_path.empty()) {
            metrics_reporter_ =
                std::make_unique<instrumentation::MetricsJsonlReporter>(
                    timers_,
                    metrics_source_,
                    std::move(metrics_path),
                    metrics_report_interval);
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
        result.configure_h264 = configure_callback;
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
        if (!started_) {
            finish_metrics();
            rethrow_if_failed();
            return;
        }
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline) {
            const OutputStatus status = manager_.status().front();
            if (status.queue_depth == 0 &&
                status.written + status.dropped >= status.published) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        manager_.stop(output_id_);
        started_ = false;
        const OutputStatus status = manager_.status().front();
        const auto write_timing = write_timer_.snapshot();
        std::cout << "VTB_OUTPUT_RESULT output_id=" << output_id_
                  << " published=" << status.published
                  << " written=" << status.written << " dropped=" << status.dropped
                  << " errors=" << status.errors << " reconnects=" << status.reconnects
                  << " queue_depth=" << status.queue_depth
                  << " queue_capacity=" << status.queue_capacity
                  << " write_samples=" << write_timing.sample_count()
                  << " write_average_ns=" << write_timing.average().count() << "\n";
        finish_metrics();
        rethrow_if_failed();
        if (status.written == 0) {
            throw std::runtime_error(
                "Media output " + output_id_ +
                " did not write any encoded packets: " + status.last_error);
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
        void* context, const H264StreamFormat* format) noexcept {
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

    void configure(const H264StreamFormat& format) {
        if (started_ || format.codec_configuration == nullptr ||
            format.codec_configuration_size == 0 ||
            format.codec_configuration_size > std::numeric_limits<std::size_t>::max()) {
            throw std::invalid_argument("Invalid or repeated encoded stream configuration");
        }
        std::vector<std::uint8_t> configuration(
            format.codec_configuration,
            format.codec_configuration +
                static_cast<std::size_t>(format.codec_configuration_size));
        manager_.register_stream(StreamDescriptor::video(
            stream_id_, 1, "h264", format.width,
            format.height, format.fps, 1, true, std::move(configuration)));
        manager_.bind(output_id_, stream_id_);
        manager_.start(output_id_);
        started_ = true;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            if (manager_.status().front().state == OutputState::running) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        throw std::runtime_error(
            "Media output did not start: " + manager_.status().front().last_error);
    }

    void publish(const EncodedVideoPacketView& view) {
        if (!started_ || view.data == nullptr || view.size == 0 ||
            view.size > std::numeric_limits<std::size_t>::max()) {
            throw std::invalid_argument("Invalid encoded video packet");
        }
        // The producer owns view.data only until this callback returns.
        auto payload = std::make_shared<const std::vector<std::uint8_t>>(
            view.data, view.data + static_cast<std::size_t>(view.size));
        MediaPacket packet(stream_id_, std::move(payload), view.pts_ns, view.dts_ns,
            view.duration_ns, view.sequence, view.keyframe != 0, false);
        // Zero enqueues is allowed during reconnect/keyframe wait/backpressure.
        manager_.publish(std::move(packet));
    }

    std::string output_id_;
    std::string stream_id_;
    std::string metrics_source_;
    instrumentation::TimerRegistry timers_;
    instrumentation::TimerHandle write_timer_;
    std::unique_ptr<instrumentation::MetricsJsonlReporter> metrics_reporter_;
    OutputManager manager_;
    mutable std::mutex error_mutex_;
    std::exception_ptr callback_error_;
    // Configure precedes media-worker start; finish follows worker join.
    bool started_ = false;
    bool metrics_finished_ = false;
};

MediaOutputSession::MediaOutputSession(
    std::unique_ptr<Output> output,
    std::string metrics_path,
    std::chrono::milliseconds metrics_report_interval,
    std::string output_id,
    std::string stream_id,
    std::string metrics_source,
    std::string write_timer_name)
    : impl_(std::make_unique<Impl>(
          std::move(output),
          std::move(metrics_path),
          metrics_report_interval,
          std::move(output_id),
          std::move(stream_id),
          std::move(metrics_source),
          std::move(write_timer_name))) {}
MediaOutputSession::~MediaOutputSession() = default;
EncodedVideoSink MediaOutputSession::sink() noexcept { return impl_->sink(); }
void MediaOutputSession::rethrow_if_failed() const { impl_->rethrow_if_failed(); }
void MediaOutputSession::finish() { impl_->finish(); }

}  // namespace vtb
