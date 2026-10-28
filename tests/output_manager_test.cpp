#include "vtb/output/output_manager.hpp"

#include <cassert>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using vtb::MediaPacket;
using vtb::Output;
using vtb::OutputManager;
using vtb::OutputOptions;
using vtb::OutputState;
using vtb::StreamDescriptor;

class FakeOutput : public Output {
public:
    explicit FakeOutput(bool fail_writes = false, bool block_first_write = false)
        : fail_writes_(fail_writes), block_first_write_(block_first_write) {}

    ~FakeOutput() override = default;
    FakeOutput(const FakeOutput&) = delete;
    FakeOutput& operator=(const FakeOutput&) = delete;
    FakeOutput(FakeOutput&&) = delete;
    FakeOutput& operator=(FakeOutput&&) = delete;

    bool supports(const StreamDescriptor& descriptor) const override {
        return descriptor.type() == vtb::MediaType::video;
    }

    void start(const std::vector<StreamDescriptor>&) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++starts_;
        start_thread_ = std::this_thread::get_id();
        stop_requested_.store(false);
        changed_.notify_all();
    }

    void write(const MediaPacket& packet) override {
        if (fail_writes_) {
            throw std::runtime_error("injected write failure");
        }
        std::unique_lock<std::mutex> lock(mutex_);
        if (block_first_write_ && sequences_.empty()) {
            write_blocked_ = true;
            changed_.notify_all();
            changed_.wait(lock, [this] {
                return release_write_ || stop_requested_.load();
            });
        }
        if (stop_requested_.load()) {
            return;
        }
        sequences_.push_back(packet.sequence());
        changed_.notify_all();
    }

    void request_stop() noexcept override {
        stop_requested_.store(true);
        changed_.notify_all();
    }

    void stop() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stops_;
        stop_thread_ = std::this_thread::get_id();
        changed_.notify_all();
    }

    std::string name() const override { return "fake"; }

    bool wait_for_writes(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, 1s, [this, count] {
            return sequences_.size() >= count;
        });
    }

    bool wait_until_write_blocked() {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, 1s, [this] { return write_blocked_; });
    }

    void release_blocked_write() {
        std::lock_guard<std::mutex> lock(mutex_);
        release_write_ = true;
        changed_.notify_all();
    }

    std::size_t writes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return sequences_.size();
    }

    std::vector<std::uint64_t> written_sequences() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return sequences_;
    }

    int starts() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return starts_;
    }

    bool stop_ran_on_start_thread() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return starts_ > 0 && start_thread_ == stop_thread_;
    }

private:
    bool fail_writes_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::uint64_t> sequences_;
    bool block_first_write_;
    bool write_blocked_ = false;
    bool release_write_ = false;
    int starts_ = 0;
    int stops_ = 0;
    std::atomic<bool> stop_requested_{false};
    std::thread::id start_thread_;
    std::thread::id stop_thread_;
};

MediaPacket packet(std::uint64_t sequence, bool keyframe = false) {
    auto payload = std::make_shared<const std::vector<std::uint8_t>>(
        std::vector<std::uint8_t>{static_cast<std::uint8_t>(sequence)});
    return MediaPacket("video", std::move(payload), sequence * 1000,
                       sequence * 1000, 1000, sequence, keyframe);
}

void test_fanout_and_lifecycle() {
    OutputManager manager;
    manager.register_stream(StreamDescriptor::video("video", 1, "H264", 640, 480,
                                                    30, 1, true));
    auto first_owner = std::make_unique<FakeOutput>();
    auto second_owner = std::make_unique<FakeOutput>();
    auto* first = first_owner.get();
    auto* second = second_owner.get();
    manager.add("first", std::move(first_owner));
    manager.add("second", std::move(second_owner));
    manager.bind("first", "video");
    manager.bind("second", "video");
    manager.start("first");
    manager.start("second");
    bool duplicate_start_rejected = false;
    try {
        manager.start("first");
    } catch (const std::logic_error&) {
        duplicate_start_rejected = true;
    }
    assert(duplicate_start_rejected);
    for (std::uint64_t index = 0; index < 4; ++index) {
        manager.publish(packet(index, index == 0));
    }
    assert(first->wait_for_writes(1));
    assert(second->wait_for_writes(1));
    manager.stop("first");
    manager.stop("second");
    assert(first->writes() > 0);
    assert(second->writes() > 0);
    assert(manager.status().size() == 2);
    assert(manager.status().front().state == OutputState::stopped);
    manager.remove("first");
    assert(manager.status().size() == 1);
}

void test_bounded_queue_and_keyframe_recovery() {
    OutputManager manager;
    manager.register_stream(StreamDescriptor::video("video", 1, "H264", 640, 480,
                                                    30, 1, true));
    auto output_owner = std::make_unique<FakeOutput>(false, true);
    auto* output = output_owner.get();
    OutputOptions options;
    options.queue_capacity = 2;
    manager.add("bounded", std::move(output_owner), options);
    manager.bind("bounded", "video");
    manager.start("bounded");
    manager.publish(packet(0, true));
    assert(output->wait_until_write_blocked());
    manager.publish(packet(1));
    manager.publish(packet(2));
    manager.publish(packet(3));
    manager.publish(packet(4, true));
    const auto queued_status = manager.status();
    assert(queued_status.front().queue_depth <= options.queue_capacity);
    output->release_blocked_write();
    assert(output->wait_for_writes(2));
    manager.stop("bounded");
    const auto final_status = manager.status();
    assert(final_status.front().dropped >= 2);
    assert(output->written_sequences() == std::vector<std::uint64_t>({0, 4}));
}

void test_failure_isolation() {
    OutputManager manager;
    manager.register_stream(StreamDescriptor::video("video", 1, "H264", 640, 480,
                                                    30, 1, true));
    OutputOptions options;
    options.max_retries = 1;
    options.retry_backoff = 1ms;
    options.max_retry_backoff = 1ms;
    auto broken_owner = std::make_unique<FakeOutput>(true);
    auto healthy_owner = std::make_unique<FakeOutput>();
    auto* broken = broken_owner.get();
    auto* healthy = healthy_owner.get();
    manager.add("broken", std::move(broken_owner), options);
    manager.add("healthy", std::move(healthy_owner));
    manager.bind("broken", "video");
    manager.bind("healthy", "video");
    manager.start("broken");
    manager.start("healthy");
    manager.publish(packet(1, true));
    assert(healthy->wait_for_writes(1));

    const auto deadline = std::chrono::steady_clock::now() + 1s;
    bool broken_error_observed = false;
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto& value : manager.status()) {
            if (value.id == "broken" && value.errors > 0 &&
                value.retry_attempts > 0) {
                broken_error_observed = true;
            }
        }
        if (broken_error_observed) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    assert(broken_error_observed);
    assert(healthy->writes() == 1);
    assert(broken->stop_ran_on_start_thread());
    assert(healthy->stop_ran_on_start_thread());
    manager.stop("broken");
    manager.stop("healthy");
}

void test_stop_wakes_blocked_write() {
    OutputManager manager;
    manager.register_stream(StreamDescriptor::video("video", 1, "H264", 640, 480,
                                                    30, 1, true));
    auto output_owner = std::make_unique<FakeOutput>(false, true);
    auto* output = output_owner.get();
    manager.add("blocking", std::move(output_owner));
    manager.bind("blocking", "video");
    manager.start("blocking");
    manager.publish(packet(0, true));
    assert(output->wait_until_write_blocked());
    manager.stop("blocking");
    assert(manager.status().front().state == OutputState::stopped);
    assert(output->stop_ran_on_start_thread());
}

void test_descriptor_validation() {
    bool rejected = false;
    try {
        StreamDescriptor::video("bad", 1, "H264", 0, 480, 30, 1, true);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);

    rejected = false;
    try {
        StreamDescriptor::audio("bad-audio", 1, "S16LE", 0, 2);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);

    const auto encoded_audio = StreamDescriptor::audio(
        "aac", 1, "AAC", 48000, 2, true, std::vector<std::uint8_t>{0x12, 0x10});
    assert(encoded_audio.type() == vtb::MediaType::audio);
    assert(encoded_audio.encoded());
    assert(encoded_audio.codec_config().size() == 2);

    rejected = false;
    try {
        MediaPacket empty("video",
                          std::make_shared<const std::vector<std::uint8_t>>(),
                          0, 0, 1, 0);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}

}  // namespace

int main() {
    test_descriptor_validation();
    test_fanout_and_lifecycle();
    test_bounded_queue_and_keyframe_recovery();
    test_failure_isolation();
    test_stop_wakes_blocked_write();
    return 0;
}
