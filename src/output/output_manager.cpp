#include "vtb/output/output_manager.hpp"

#include <condition_variable>
#include <deque>
#include <exception>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

#include "vtb/time/monotonic_clock.hpp"

namespace vtb {
namespace {

std::chrono::milliseconds retry_delay(const OutputOptions& options,
                                      std::uint32_t retry_number) {
    auto delay = options.retry_backoff;
    for (std::uint32_t index = 1;
         index < retry_number && delay < options.max_retry_backoff; ++index) {
        if (delay.count() > options.max_retry_backoff.count() / 2) {
            return options.max_retry_backoff;
        }
        delay *= 2;
    }
    if (delay > options.max_retry_backoff) {
        return options.max_retry_backoff;
    }
    return delay;
}

class OutputEntry : public std::enable_shared_from_this<OutputEntry> {
public:
    OutputEntry(std::string output_id, std::unique_ptr<Output> output_value,
                OutputOptions output_options)
        : id(std::move(output_id)), output(std::move(output_value)),
          name(output->name()), options(output_options) {}

    ~OutputEntry() {
        request_stop();
        join_worker();
    }

    OutputEntry(const OutputEntry&) = delete;
    OutputEntry& operator=(const OutputEntry&) = delete;
    OutputEntry(OutputEntry&&) = delete;
    OutputEntry& operator=(OutputEntry&&) = delete;

    void request_stop() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stop_requested = true;
            dropped += queue.size();
            queue.clear();
            queue_changed.notify_all();
        }
        output->request_stop();
    }

    void join_worker() {
        if (worker.joinable()) {
            if (worker.get_id() == std::this_thread::get_id()) {
                throw std::logic_error("an output worker cannot join itself");
            }
            worker.join();
        }
    }

    void run() noexcept {
        std::uint32_t retries = 0;
        bool output_started = false;
        bool has_started_once = false;
        while (true) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (stop_requested) {
                    break;
                }
                state = OutputState::starting;
            }

            try {
                output_started = true;
                output->start(start_descriptors);
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (stop_requested) {
                        break;
                    }
                    if (has_started_once) {
                        ++reconnects;
                    }
                    has_started_once = true;
                    state = OutputState::running;
                    queue_changed.notify_all();
                }

                while (true) {
                    std::optional<MediaPacket> packet = take_packet();
                    if (!packet.has_value()) {
                        break;
                    }
                    const std::string failed_stream = packet->stream_id();
                    try {
                        output->write(std::move(*packet));
                        const std::uint64_t success_ns =
                            MonotonicClock::now_ns();
                        std::lock_guard<std::mutex> lock(mutex);
                        ++written;
                        last_success_ns = success_ns;
                        retries = 0;
                    } catch (const std::exception& error) {
                        record_error(error.what(), failed_stream);
                        output_started = false;
                        output->stop();
                        break;
                    } catch (...) {
                        record_error("unknown exception from output write", failed_stream);
                        output_started = false;
                        output->stop();
                        break;
                    }
                }

                if (should_stop()) {
                    break;
                }
            } catch (const std::exception& error) {
                record_error(error.what(), {});
            } catch (...) {
                record_error("unknown exception from output start", {});
            }

            if (output_started) {
                output->stop();
                output_started = false;
            }

            if (retries < std::numeric_limits<std::uint32_t>::max()) {
                ++retries;
            }
            if (options.max_retries != 0 && retries > options.max_retries) {
                std::lock_guard<std::mutex> lock(mutex);
                state = OutputState::failed;
                dropped += queue.size();
                queue.clear();
                queue_changed.notify_all();
                return;
            }

            std::unique_lock<std::mutex> lock(mutex);
            state = OutputState::reconnecting;
            ++retry_attempts;
            const auto delay = retry_delay(options, retries);
            queue_changed.wait_for(lock, delay,
                                   [this] { return stop_requested; });
            if (stop_requested) {
                break;
            }
        }

        if (output_started) {
            output->stop();
        }
        std::lock_guard<std::mutex> lock(mutex);
        state = OutputState::stopped;
    }

    std::optional<MediaPacket> take_packet() {
        std::unique_lock<std::mutex> lock(mutex);
        queue_changed.wait(lock, [this] { return stop_requested || !queue.empty(); });
        if (stop_requested || queue.empty()) {
            return std::nullopt;
        }
        MediaPacket packet = std::move(queue.front());
        queue.pop_front();
        queue_changed.notify_all();
        return std::optional<MediaPacket>(std::move(packet));
    }

    bool should_stop() {
        std::lock_guard<std::mutex> lock(mutex);
        return stop_requested;
    }

    void record_error(const std::string& message, const std::string& failed_stream) {
        std::lock_guard<std::mutex> lock(mutex);
        ++errors;
        state = OutputState::reconnecting;
        last_error = message;
        if (!failed_stream.empty()) {
            ++dropped;
            const auto descriptor = descriptors.find(failed_stream);
            if (descriptor != descriptors.end() &&
                descriptor->second.type() == MediaType::video &&
                descriptor->second.encoded()) {
                wait_keyframe.insert(failed_stream);
                pending_discontinuity.insert(failed_stream);
                discard_until_keyframe(failed_stream);
            }
        }
    }

    bool enqueue(MediaPacket source) {
        std::lock_guard<std::mutex> lock(mutex);
        if (stop_requested) {
            return false;
        }
        ++published;
        if (state != OutputState::starting && state != OutputState::running) {
            ++dropped;
            return false;
        }
        const auto descriptor = descriptors.find(source.stream_id());
        if (descriptor == descriptors.end()) {
            ++dropped;
            return false;
        }
        const bool compressed_video = descriptor->second.type() == MediaType::video &&
                                      descriptor->second.encoded();
        if (compressed_video && wait_keyframe.count(source.stream_id()) != 0 &&
            !source.keyframe()) {
            ++dropped;
            pending_discontinuity.insert(source.stream_id());
            return false;
        }
        if (compressed_video && source.keyframe()) {
            wait_keyframe.erase(source.stream_id());
        }

        if (queue.size() >= options.queue_capacity) {
            const std::string removed_stream = queue.front().stream_id();
            queue.pop_front();
            ++dropped;
            pending_discontinuity.insert(removed_stream);
            const auto removed_descriptor = descriptors.find(removed_stream);
            if (removed_descriptor != descriptors.end() &&
                removed_descriptor->second.type() == MediaType::video &&
                removed_descriptor->second.encoded()) {
                wait_keyframe.insert(removed_stream);
                discard_until_keyframe(removed_stream);
            }
            if (compressed_video && wait_keyframe.count(source.stream_id()) != 0 &&
                !source.keyframe()) {
                ++dropped;
                return false;
            }
            if (compressed_video && source.keyframe()) {
                wait_keyframe.erase(source.stream_id());
            }
        }

        if (pending_discontinuity.erase(source.stream_id()) != 0 &&
            !source.discontinuity()) {
            queue.push_back(std::move(source).with_discontinuity());
        } else {
            queue.push_back(std::move(source));
        }
        queue_changed.notify_one();
        return true;
    }

    void discard_until_keyframe(const std::string& stream_id) {
        bool found_keyframe = false;
        for (auto it = queue.begin(); it != queue.end();) {
            if (it->stream_id() != stream_id) {
                ++it;
                continue;
            }
            if (it->keyframe()) {
                if (!it->discontinuity()) {
                    *it = std::move(*it).with_discontinuity();
                }
                pending_discontinuity.erase(stream_id);
                found_keyframe = true;
                break;
            }
            it = queue.erase(it);
            ++dropped;
        }
        if (found_keyframe) {
            wait_keyframe.erase(stream_id);
        }
    }

    std::string id;
    std::unique_ptr<Output> output;
    std::string name;
    OutputOptions options;
    mutable std::mutex mutex;
    std::mutex control_mutex;
    std::condition_variable queue_changed;
    std::deque<MediaPacket> queue;
    std::set<std::string> bindings;
    std::map<std::string, StreamDescriptor> descriptors;
    std::vector<StreamDescriptor> start_descriptors;
    std::set<std::string> wait_keyframe;
    std::set<std::string> pending_discontinuity;
    std::thread worker;
    OutputState state = OutputState::stopped;
    bool stop_requested = false;
    bool removed = false;
    std::uint64_t published = 0;
    std::uint64_t written = 0;
    std::uint64_t dropped = 0;
    std::uint64_t errors = 0;
    std::uint64_t retry_attempts = 0;
    std::uint64_t reconnects = 0;
    std::uint64_t last_success_ns = 0;
    std::string last_error;
};

}  // namespace

class OutputManager::Impl {
public:
    mutable std::mutex mutex;
    std::map<std::string, StreamDescriptor> streams;
    std::map<std::string, std::shared_ptr<OutputEntry>> outputs;
};

OutputManager::OutputManager() : impl_(std::make_unique<Impl>()) {}

OutputManager::~OutputManager() {
    std::vector<std::shared_ptr<OutputEntry>> entries;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        for (const auto& item : impl_->outputs) {
            entries.push_back(item.second);
        }
    }
    for (const auto& entry : entries) {
        std::lock_guard<std::mutex> control(entry->control_mutex);
        entry->request_stop();
        entry->join_worker();
    }
}

void OutputManager::register_stream(const StreamDescriptor& descriptor) {
    descriptor.validate();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->streams.count(descriptor.id()) != 0) {
        throw std::invalid_argument("stream id is already registered: " + descriptor.id());
    }
    impl_->streams.emplace(descriptor.id(), descriptor);
}

void OutputManager::add(std::string output_id, std::unique_ptr<Output> output,
                        OutputOptions options) {
    if (output_id.empty() || !output) {
        throw std::invalid_argument("output id and output instance are required");
    }
    if (options.queue_capacity == 0 || options.retry_backoff.count() < 0 ||
        options.max_retry_backoff < options.retry_backoff) {
        throw std::invalid_argument("invalid queue capacity or retry backoff range");
    }
    auto entry = std::make_shared<OutputEntry>(std::move(output_id),
                                               std::move(output), options);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->outputs.count(entry->id) != 0) {
        throw std::invalid_argument("output id is already registered: " + entry->id);
    }
    impl_->outputs.emplace(entry->id, std::move(entry));
}

void OutputManager::bind(const std::string& output_id, const std::string& stream_id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto output = impl_->outputs.find(output_id);
    const auto stream = impl_->streams.find(stream_id);
    if (output == impl_->outputs.end() || stream == impl_->streams.end()) {
        throw std::invalid_argument("output or stream is not registered");
    }
    std::lock_guard<std::mutex> output_lock(output->second->mutex);
    if (output->second->state != OutputState::stopped) {
        throw std::logic_error("bindings can only change while an output is stopped");
    }
    if (!output->second->bindings.insert(stream_id).second) {
        throw std::invalid_argument("stream is already bound to output");
    }
}

void OutputManager::unbind(const std::string& output_id, const std::string& stream_id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto output = impl_->outputs.find(output_id);
    if (output == impl_->outputs.end()) {
        throw std::invalid_argument("output is not registered: " + output_id);
    }
    std::lock_guard<std::mutex> output_lock(output->second->mutex);
    if (output->second->state != OutputState::stopped) {
        throw std::logic_error("bindings can only change while an output is stopped");
    }
    if (output->second->bindings.erase(stream_id) == 0) {
        throw std::invalid_argument("stream is not bound to output");
    }
}

void OutputManager::start(const std::string& output_id) {
    std::shared_ptr<OutputEntry> entry;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto found = impl_->outputs.find(output_id);
        if (found == impl_->outputs.end()) {
            throw std::invalid_argument("output is not registered: " + output_id);
        }
        entry = found->second;
    }
    std::lock_guard<std::mutex> control(entry->control_mutex);
    if (entry->removed) {
        throw std::logic_error("output has been removed");
    }
    {
        std::lock_guard<std::mutex> lock(entry->mutex);
        if (entry->state != OutputState::stopped && entry->state != OutputState::failed) {
            throw std::logic_error("output is already active");
        }
    }
    entry->join_worker();

    std::vector<StreamDescriptor> descriptors;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        std::lock_guard<std::mutex> output_lock(entry->mutex);
        for (const std::string& stream_id : entry->bindings) {
            const auto stream = impl_->streams.find(stream_id);
            if (stream == impl_->streams.end()) {
                throw std::logic_error("bound stream is no longer registered");
            }
            if (!entry->output->supports(stream->second)) {
                throw std::invalid_argument("output does not support stream: " + stream_id);
            }
            descriptors.push_back(stream->second);
        }
        if (descriptors.empty()) {
            throw std::logic_error("output has no bound streams");
        }
        entry->start_descriptors = descriptors;
        entry->descriptors.clear();
        for (const auto& descriptor : descriptors) {
            entry->descriptors.emplace(descriptor.id(), descriptor);
        }
        entry->queue.clear();
        entry->stop_requested = false;
        entry->wait_keyframe.clear();
        entry->pending_discontinuity.clear();
        entry->state = OutputState::starting;
    }
    try {
        entry->worker = std::thread([entry] { entry->run(); });
    } catch (...) {
        std::lock_guard<std::mutex> lock(entry->mutex);
        entry->state = OutputState::stopped;
        throw;
    }
}

void OutputManager::stop(const std::string& output_id) {
    std::shared_ptr<OutputEntry> entry;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto found = impl_->outputs.find(output_id);
        if (found == impl_->outputs.end()) {
            throw std::invalid_argument("output is not registered: " + output_id);
        }
        entry = found->second;
    }
    std::lock_guard<std::mutex> control(entry->control_mutex);
    entry->request_stop();
    entry->join_worker();
    std::lock_guard<std::mutex> lock(entry->mutex);
    entry->state = OutputState::stopped;
}

void OutputManager::remove(const std::string& output_id) {
    std::shared_ptr<OutputEntry> entry;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto found = impl_->outputs.find(output_id);
        if (found == impl_->outputs.end()) {
            throw std::invalid_argument("output is not registered: " + output_id);
        }
        entry = found->second;
    }
    std::lock_guard<std::mutex> control(entry->control_mutex);
    entry->request_stop();
    entry->join_worker();
    entry->removed = true;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->outputs.erase(output_id);
}

std::size_t OutputManager::publish(MediaPacket packet) {
    std::vector<std::shared_ptr<OutputEntry>> outputs;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->streams.count(packet.stream_id()) == 0) {
            throw std::invalid_argument("packet refers to an unregistered stream");
        }
        for (const auto& item : impl_->outputs) {
            if (item.second->bindings.count(packet.stream_id()) != 0) {
                outputs.push_back(item.second);
            }
        }
    }
    std::size_t queued = 0;
    for (std::size_t index = 0; index < outputs.size(); ++index) {
        bool accepted = false;
        if (index + 1 == outputs.size()) {
            accepted = outputs[index]->enqueue(std::move(packet));
        } else {
            accepted = outputs[index]->enqueue(packet);
        }
        if (accepted) {
            ++queued;
        }
    }
    return queued;
}

std::vector<OutputStatus> OutputManager::status() const {
    std::vector<std::pair<std::shared_ptr<OutputEntry>, std::vector<std::string>>> outputs;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        for (const auto& item : impl_->outputs) {
            outputs.emplace_back(item.second,
                                 std::vector<std::string>(item.second->bindings.begin(),
                                                          item.second->bindings.end()));
        }
    }
    std::vector<OutputStatus> result;
    result.reserve(outputs.size());
    for (const auto& item : outputs) {
        const auto& output = item.first;
        std::lock_guard<std::mutex> lock(output->mutex);
        OutputStatus value;
        value.id = output->id;
        value.name = output->name;
        value.streams = item.second;
        value.state = output->state;
        value.queue_depth = output->queue.size();
        value.queue_capacity = output->options.queue_capacity;
        value.published = output->published;
        value.written = output->written;
        value.dropped = output->dropped;
        value.errors = output->errors;
        value.retry_attempts = output->retry_attempts;
        value.reconnects = output->reconnects;
        value.last_success_ns = output->last_success_ns;
        value.last_error = output->last_error;
        result.push_back(std::move(value));
    }
    return result;
}

}  // namespace vtb
