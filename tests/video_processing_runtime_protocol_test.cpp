#include "vtb/pipeline/video_processing_runtime.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

class RecordedEvents final {
public:
    RecordedEvents() = default;
    ~RecordedEvents() = default;

    RecordedEvents(const RecordedEvents&) = delete;
    RecordedEvents& operator=(const RecordedEvents&) = delete;
    RecordedEvents(RecordedEvents&&) = delete;
    RecordedEvents& operator=(RecordedEvents&&) = delete;

    void append(std::string event) {
        std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back(std::move(event));
    }

    std::vector<std::string> snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return events_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> events_;
};

class OneFrameSource final : public vtb::VideoSource {
public:
    OneFrameSource() = default;
    ~OneFrameSource() override = default;

    OneFrameSource(const OneFrameSource&) = delete;
    OneFrameSource& operator=(const OneFrameSource&) = delete;
    OneFrameSource(OneFrameSource&&) = delete;
    OneFrameSource& operator=(OneFrameSource&&) = delete;

    std::optional<vtb::DecodedFrameLease> read() override {
        if (stop_requested_.load() || delivered_) {
            return std::nullopt;
        }
        delivered_ = true;
        std::array<vtb::FramePlaneView, vtb::FrameView::max_planes> planes{};
        planes[0] = vtb::FramePlaneView(
            pixels_.data(), pixels_.size(), 3, -1, 0);
        vtb::FrameView view(
            vtb::VideoPixelFormat::rgb888,
            vtb::FrameMemoryType::cpu,
            1,
            1,
            1,
            1,
            vtb::FrameTimestampSource::dequeue_monotonic,
            planes,
            1);
        return vtb::DecodedFrameLease(std::move(view), []() {});
    }

    void request_stop() noexcept override {
        stop_requested_.store(true);
    }

    void close() noexcept override {
        request_stop();
    }

private:
    std::array<std::byte, 3> pixels_{};
    std::atomic<bool> stop_requested_{false};
    bool delivered_ = false;
};

class RecordingConsumer final : public vtb::VideoFrameConsumer {
public:
    explicit RecordingConsumer(std::shared_ptr<RecordedEvents> events)
        : events_(std::move(events)) {}
    ~RecordingConsumer() override = default;

    RecordingConsumer(const RecordingConsumer&) = delete;
    RecordingConsumer& operator=(const RecordingConsumer&) = delete;
    RecordingConsumer(RecordingConsumer&&) = delete;
    RecordingConsumer& operator=(RecordingConsumer&&) = delete;

    void begin_source(vtb::VideoStreamGeneration generation) override {
        assert(generation.value() == 1);
        events_->append("begin");
    }

    void consume(vtb::DecodedFrameLease frame) override {
        assert(frame.view().frame_id() == 1);
        events_->append("frame");
    }

    void retire_source(
        vtb::VideoStreamGeneration generation) noexcept override {
        assert(generation.value() == 1);
        events_->append("retire");
    }

private:
    std::shared_ptr<RecordedEvents> events_;
};

}  // namespace

int main() {
    auto events = std::make_shared<RecordedEvents>();
    vtb::VideoProcessingRuntime runtime(
        std::make_unique<OneFrameSource>(),
        std::make_unique<RecordingConsumer>(events));
    runtime.start();
    runtime.wait();
    runtime.rethrow_if_failed();

    const std::vector<std::string> recorded = events->snapshot();
    assert(recorded.size() == 3);
    assert(recorded[0] == "begin");
    assert(recorded[1] == "frame");
    assert(recorded[2] == "retire");
    return 0;
}
