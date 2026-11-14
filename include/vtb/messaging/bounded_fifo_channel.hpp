#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace vtb::messaging {

/** @brief 容量已满时采用的生产者策略。 */
enum class ChannelOverflowPolicy {
    /** @brief 阻塞生产者，直到消费者腾出空间或通道停止。 */
    block,
    /** @brief 释放队首最旧消息，再接收当前消息。 */
    drop_oldest,
    /** @brief 保留已有消息，释放当前新消息。 */
    drop_newest,
};

/** @brief publish() 的所有可观察结果。 */
enum class ChannelPublishResult {
    /** @brief 当前消息已进入队列。 */
    published,
    /** @brief 最旧消息已被释放，当前消息已进入队列。 */
    published_after_dropping_oldest,
    /** @brief 队列已满，当前消息按策略被释放。 */
    dropped_newest,
    /** @brief finish() 已调用，通道不再接收消息。 */
    finished,
    /** @brief close() 已调用，通道已中止。 */
    closed,
};

/**
 * @brief 在线程之间移动消息所有权的有界 FIFO 通道。
 *
 * 通道只依赖消息类型 T 的移动构造和析构能力。生产者调用 publish() 后，成功
 * 入队的消息由通道拥有；消费者通过 wait_pop() 或 try_pop() 取走消息后获得
 * 所有权。通道不会调用生产者或消费者，也不要求 T 继承公共基类。
 *
 * 当消息因覆盖、拒绝或 close() 被移除时，通道会先在互斥锁保护下转移消息，
 * 再在锁外析构。对于 DecodedFrameLease 这类 RAII 消息，析构会自然触发其资源
 * 归还逻辑，同时避免在归还资源期间占用通道锁。
 *
 * finish() 表示正常结束：已入队消息仍可按 FIFO 顺序取走，队列排空后消费接口
 * 返回 std::nullopt。close() 表示立即中止：尚未取走的消息会被释放，所有等待
 * 中的生产者和消费者都会被唤醒。
 *
 * @tparam T 可移动构造、可析构的消息类型。
 */
template <typename T>
class BoundedFifoChannel final {
    static_assert(std::is_move_constructible<T>::value,
                  "BoundedFifoChannel<T> requires move construction");
    static_assert(std::is_destructible<T>::value,
                  "BoundedFifoChannel<T> requires destruction");

public:
    /**
     * @param capacity 队列最大消息数，必须大于零。
     * @param overflow_policy 队列已满时采用的处理策略。
     */
    explicit BoundedFifoChannel(
        std::size_t capacity,
        ChannelOverflowPolicy overflow_policy)
        : capacity_(capacity), overflow_policy_(overflow_policy) {
        if (capacity_ == 0) {
            throw std::invalid_argument(
                "BoundedFifoChannel capacity must be greater than zero");
        }
    }

    /** @brief 中止通道，并在锁外释放仍由通道拥有的消息。 */
    ~BoundedFifoChannel() noexcept {
        close();
    }

    BoundedFifoChannel(const BoundedFifoChannel&) = delete;
    BoundedFifoChannel& operator=(const BoundedFifoChannel&) = delete;
    BoundedFifoChannel(BoundedFifoChannel&&) = delete;
    BoundedFifoChannel& operator=(BoundedFifoChannel&&) = delete;

    /**
     * @brief 把消息所有权交给通道，或按满载策略释放消息。
     *
     * block 策略可能阻塞，直到出现空位、finish() 或 close()。其余策略不会等待
     * 容量。返回值明确说明消息是否入队以及通道是否已经停止。
     */
    [[nodiscard]] ChannelPublishResult publish(T message) {
        std::optional<T> discarded;
        std::unique_lock<std::mutex> lock(mutex_);

        if (overflow_policy_ == ChannelOverflowPolicy::block) {
            not_full_.wait(lock, [this]() {
                return closed_ || finished_ || queue_.size() < capacity_;
            });
        }

        if (closed_) {
            lock.unlock();
            return ChannelPublishResult::closed;
        }
        if (finished_) {
            lock.unlock();
            return ChannelPublishResult::finished;
        }

        if (queue_.size() == capacity_) {
            if (overflow_policy_ == ChannelOverflowPolicy::drop_newest) {
                ++dropped_count_;
                lock.unlock();
                return ChannelPublishResult::dropped_newest;
            }

            if (overflow_policy_ == ChannelOverflowPolicy::drop_oldest) {
                discarded.emplace(std::move(queue_.front()));
                queue_.pop_front();
                ++dropped_count_;
                queue_.push_back(std::move(message));
                ++published_count_;
                lock.unlock();
                not_empty_.notify_one();
                return ChannelPublishResult::published_after_dropping_oldest;
            }
        }

        queue_.push_back(std::move(message));
        ++published_count_;
        lock.unlock();
        not_empty_.notify_one();
        return ChannelPublishResult::published;
    }

    /**
     * @brief 等待并取走队首消息。
     * @return 取得的消息；通道结束且没有剩余消息时返回 std::nullopt。
     */
    [[nodiscard]] std::optional<T> wait_pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [this]() {
            return closed_ || finished_ || !queue_.empty();
        });

        if (queue_.empty()) {
            return std::nullopt;
        }

        std::optional<T> result(
            std::in_place, std::move(queue_.front()));
        queue_.pop_front();
        ++popped_count_;
        lock.unlock();
        not_full_.notify_one();
        return result;
    }

    /**
     * @brief 尝试立即取走队首消息，不等待生产者。
     * @return 当前队首消息；队列为空时返回 std::nullopt。
     */
    [[nodiscard]] std::optional<T> try_pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return std::nullopt;
        }

        std::optional<T> result(
            std::in_place, std::move(queue_.front()));
        queue_.pop_front();
        ++popped_count_;
        lock.unlock();
        not_full_.notify_one();
        return result;
    }

    /**
     * @brief 停止接收新消息，并允许消费者排空已有消息。
     *
     * 该操作可重复调用，并会唤醒所有等待中的生产者和消费者。
     */
    void finish() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            finished_ = true;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    /**
     * @brief 立即中止通道，并释放尚未取走的全部消息。
     *
     * 被丢弃消息的析构发生在互斥锁释放后。该操作可重复调用。
     */
    void close() noexcept {
        std::deque<T> abandoned;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) {
                return;
            }
            closed_ = true;
            finished_ = true;
            dropped_count_ += queue_.size();
            abandoned.swap(queue_);
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }

    [[nodiscard]] ChannelOverflowPolicy overflow_policy() const noexcept {
        return overflow_policy_;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    [[nodiscard]] bool finished() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return finished_;
    }

    [[nodiscard]] bool closed() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    [[nodiscard]] std::uint64_t published_count() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return published_count_;
    }

    [[nodiscard]] std::uint64_t popped_count() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return popped_count_;
    }

    [[nodiscard]] std::uint64_t dropped_count() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_count_;
    }

private:
    const std::size_t capacity_;
    const ChannelOverflowPolicy overflow_policy_;

    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::deque<T> queue_;
    bool finished_ = false;
    bool closed_ = false;
    std::uint64_t published_count_ = 0;
    std::uint64_t popped_count_ = 0;
    std::uint64_t dropped_count_ = 0;
};

}  // namespace vtb::messaging
