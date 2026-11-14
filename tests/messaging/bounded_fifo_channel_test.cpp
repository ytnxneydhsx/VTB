#include "vtb/messaging/bounded_fifo_channel.hpp"

#include <atomic>
#include <optional>
#include <stdexcept>
#include <thread>

namespace {

void require(bool condition) {
    if (!condition) {
        throw std::runtime_error("bounded FIFO channel check failed");
    }
}

class MoveOnlyMessage final {
public:
    MoveOnlyMessage(int value, std::atomic<int>& release_count) noexcept
        : value_(value), release_count_(&release_count) {}

    ~MoveOnlyMessage() noexcept {
        release();
    }

    MoveOnlyMessage(const MoveOnlyMessage&) = delete;
    MoveOnlyMessage& operator=(const MoveOnlyMessage&) = delete;

    MoveOnlyMessage(MoveOnlyMessage&& other) noexcept
        : value_(other.value_), release_count_(other.release_count_) {
        other.release_count_ = nullptr;
    }

    MoveOnlyMessage& operator=(MoveOnlyMessage&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        release();
        value_ = other.value_;
        release_count_ = other.release_count_;
        other.release_count_ = nullptr;
        return *this;
    }

    [[nodiscard]] int value() const noexcept {
        return value_;
    }

private:
    void release() noexcept {
        if (release_count_ == nullptr) {
            return;
        }
        release_count_->fetch_add(1);
        release_count_ = nullptr;
    }

    int value_ = 0;
    std::atomic<int>* release_count_ = nullptr;
};

void verify_capacity_validation() {
    bool rejected = false;
    try {
        vtb::messaging::BoundedFifoChannel<int> channel(
            0, vtb::messaging::ChannelOverflowPolicy::block);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected);
}

void verify_drop_oldest_moves_ownership() {
    std::atomic<int> release_count{0};
    vtb::messaging::BoundedFifoChannel<MoveOnlyMessage> channel(
        1, vtb::messaging::ChannelOverflowPolicy::drop_oldest);

    require(channel.publish(MoveOnlyMessage(1, release_count)) ==
            vtb::messaging::ChannelPublishResult::published);
    require(channel.publish(MoveOnlyMessage(2, release_count)) ==
            vtb::messaging::ChannelPublishResult::
                published_after_dropping_oldest);
    require(release_count.load() == 1);
    require(channel.published_count() == 2);
    require(channel.dropped_count() == 1);

    std::optional<MoveOnlyMessage> message = channel.try_pop();
    require(message.has_value());
    require(message->value() == 2);
    require(channel.popped_count() == 1);
    message.reset();
    require(release_count.load() == 2);
}

void verify_drop_newest_preserves_fifo() {
    std::atomic<int> release_count{0};
    vtb::messaging::BoundedFifoChannel<MoveOnlyMessage> channel(
        2, vtb::messaging::ChannelOverflowPolicy::drop_newest);

    require(channel.publish(MoveOnlyMessage(1, release_count)) ==
            vtb::messaging::ChannelPublishResult::published);
    require(channel.publish(MoveOnlyMessage(2, release_count)) ==
            vtb::messaging::ChannelPublishResult::published);
    require(channel.publish(MoveOnlyMessage(3, release_count)) ==
            vtb::messaging::ChannelPublishResult::dropped_newest);
    require(release_count.load() == 1);

    std::optional<MoveOnlyMessage> first = channel.try_pop();
    std::optional<MoveOnlyMessage> second = channel.try_pop();
    require(first.has_value());
    require(second.has_value());
    require(first->value() == 1);
    require(second->value() == 2);
    require(!channel.try_pop().has_value());
}

void verify_finish_drains_messages() {
    vtb::messaging::BoundedFifoChannel<int> channel(
        2, vtb::messaging::ChannelOverflowPolicy::block);
    require(channel.publish(10) ==
            vtb::messaging::ChannelPublishResult::published);
    channel.finish();
    require(channel.publish(20) ==
            vtb::messaging::ChannelPublishResult::finished);

    std::optional<int> message = channel.wait_pop();
    require(message.has_value());
    require(*message == 10);
    require(!channel.wait_pop().has_value());
}

void verify_close_releases_pending_messages() {
    std::atomic<int> release_count{0};
    vtb::messaging::BoundedFifoChannel<MoveOnlyMessage> channel(
        2, vtb::messaging::ChannelOverflowPolicy::block);
    require(channel.publish(MoveOnlyMessage(1, release_count)) ==
            vtb::messaging::ChannelPublishResult::published);
    require(channel.publish(MoveOnlyMessage(2, release_count)) ==
            vtb::messaging::ChannelPublishResult::published);

    channel.close();
    require(channel.closed());
    require(channel.finished());
    require(channel.size() == 0);
    require(channel.dropped_count() == 2);
    require(release_count.load() == 2);
    require(!channel.wait_pop().has_value());
}

void verify_blocking_producer_resumes_after_pop() {
    vtb::messaging::BoundedFifoChannel<int> channel(
        1, vtb::messaging::ChannelOverflowPolicy::block);
    require(channel.publish(1) ==
            vtb::messaging::ChannelPublishResult::published);

    std::atomic<bool> second_publish_succeeded{false};
    std::thread producer([&channel, &second_publish_succeeded]() {
        const vtb::messaging::ChannelPublishResult result =
            channel.publish(2);
        if (result == vtb::messaging::ChannelPublishResult::published) {
            second_publish_succeeded.store(true);
        }
    });

    std::optional<int> first = channel.wait_pop();
    require(first.has_value());
    require(*first == 1);
    producer.join();
    require(second_publish_succeeded.load());

    std::optional<int> second = channel.wait_pop();
    require(second.has_value());
    require(*second == 2);
}

}  // namespace

int main() {
    verify_capacity_validation();
    verify_drop_oldest_moves_ownership();
    verify_drop_newest_preserves_fifo();
    verify_finish_drains_messages();
    verify_close_releases_pending_messages();
    verify_blocking_producer_resumes_after_pop();
    return 0;
}
