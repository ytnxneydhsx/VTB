#include "vtb/instrumentation/timer.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace vtb::instrumentation {

namespace detail {

class TimerState {
public:
    explicit TimerState(std::string name) : name_(std::move(name)) {}

    ~TimerState() = default;
    TimerState(const TimerState&) = delete;
    TimerState& operator=(const TimerState&) = delete;
    TimerState(TimerState&&) = delete;
    TimerState& operator=(TimerState&&) = delete;

    const std::string& name() const noexcept { return name_; }

    std::uint64_t generation() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return generation_;
    }

    bool observe(std::chrono::nanoseconds duration,
                 std::uint64_t expected_generation) noexcept {
        if (duration.count() < 0) {
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (generation_ != expected_generation) {
            return false;
        }
        add_sample(duration);
        return true;
    }

    bool observe_current(std::chrono::nanoseconds duration) noexcept {
        if (duration.count() < 0) {
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        add_sample(duration);
        return true;
    }

    TimerSnapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::chrono::nanoseconds average{0};
        if (sample_count_ > 0) {
            average = std::chrono::nanoseconds{
                total_.count() / static_cast<std::int64_t>(sample_count_)};
        }
        return TimerSnapshot(name_, sample_count_, latest_, total_, minimum_,
                             maximum_, average);
    }

    void reset() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_;
        sample_count_ = 0;
        latest_ = std::chrono::nanoseconds{0};
        total_ = std::chrono::nanoseconds{0};
        minimum_ = std::chrono::nanoseconds{0};
        maximum_ = std::chrono::nanoseconds{0};
    }

private:
    void add_sample(std::chrono::nanoseconds duration) noexcept {
        latest_ = duration;
        total_ += duration;
        if (sample_count_ == 0) {
            minimum_ = duration;
            maximum_ = duration;
        } else {
            if (duration < minimum_) {
                minimum_ = duration;
            }
            if (duration > maximum_) {
                maximum_ = duration;
            }
        }
        ++sample_count_;
    }

    std::string name_;
    mutable std::mutex mutex_;
    std::uint64_t generation_ = 0;
    std::uint64_t sample_count_ = 0;
    std::chrono::nanoseconds latest_{0};
    std::chrono::nanoseconds total_{0};
    std::chrono::nanoseconds minimum_{0};
    std::chrono::nanoseconds maximum_{0};
};

}  // namespace detail

TimerSnapshot::TimerSnapshot(std::string name, std::uint64_t sample_count,
                             std::chrono::nanoseconds latest,
                             std::chrono::nanoseconds total,
                             std::chrono::nanoseconds minimum,
                             std::chrono::nanoseconds maximum,
                             std::chrono::nanoseconds average)
    : name_(std::move(name)),
      sample_count_(sample_count),
      latest_(latest),
      total_(total),
      minimum_(minimum),
      maximum_(maximum),
      average_(average) {}

const std::string& TimerSnapshot::name() const noexcept { return name_; }

bool TimerSnapshot::has_samples() const noexcept { return sample_count_ > 0; }

std::uint64_t TimerSnapshot::sample_count() const noexcept {
    return sample_count_;
}

std::chrono::nanoseconds TimerSnapshot::latest() const noexcept {
    return latest_;
}

std::chrono::nanoseconds TimerSnapshot::total() const noexcept { return total_; }

std::chrono::nanoseconds TimerSnapshot::minimum() const noexcept {
    return minimum_;
}

std::chrono::nanoseconds TimerSnapshot::maximum() const noexcept {
    return maximum_;
}

std::chrono::nanoseconds TimerSnapshot::average() const noexcept {
    return average_;
}

TimerHandle::TimerHandle(TimerRegistry* registry,
                         detail::TimerState* state) noexcept
    : registry_(registry), state_(state) {}

ManualTimer TimerHandle::start() const noexcept { return registry_->start(state_); }

ScopedTimer TimerHandle::scope() const noexcept { return ScopedTimer(start()); }

bool TimerHandle::observe(std::chrono::nanoseconds duration) const noexcept {
    return registry_->observe(state_, duration);
}

TimerSnapshot TimerHandle::snapshot() const { return state_->snapshot(); }

void TimerHandle::reset() const noexcept { state_->reset(); }

const std::string& TimerHandle::name() const noexcept { return state_->name(); }

ManualTimer::ManualTimer() noexcept
    : registry_(nullptr),
      state_(nullptr),
      started_at_(),
      timer_generation_(0),
      registry_generation_(0),
      active_(false) {}

ManualTimer::ManualTimer(const TimerRegistry* registry, detail::TimerState* state,
                         Clock::time_point started_at,
                         std::uint64_t timer_generation,
                         std::uint64_t registry_generation) noexcept
    : registry_(registry),
      state_(state),
      started_at_(started_at),
      timer_generation_(timer_generation),
      registry_generation_(registry_generation),
      active_(true) {}

ManualTimer::ManualTimer(ManualTimer&& other) noexcept
    : registry_(other.registry_),
      state_(other.state_),
      started_at_(other.started_at_),
      timer_generation_(other.timer_generation_),
      registry_generation_(other.registry_generation_),
      active_(other.active_) {
    other.registry_ = nullptr;
    other.state_ = nullptr;
    other.active_ = false;
}

ManualTimer& ManualTimer::operator=(ManualTimer&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    registry_ = other.registry_;
    state_ = other.state_;
    started_at_ = other.started_at_;
    timer_generation_ = other.timer_generation_;
    registry_generation_ = other.registry_generation_;
    active_ = other.active_;
    other.registry_ = nullptr;
    other.state_ = nullptr;
    other.active_ = false;
    return *this;
}

bool ManualTimer::stop() noexcept {
    if (!active_) {
        return false;
    }
    active_ = false;
    return registry_->finish(state_, started_at_, timer_generation_,
                             registry_generation_);
}

bool ManualTimer::active() const noexcept { return active_; }

ScopedTimer::ScopedTimer(ManualTimer timer) noexcept : timer_(std::move(timer)) {}

ScopedTimer::~ScopedTimer() { timer_.stop(); }

TimerRegistry::TimerRegistry() : enabled_(true), generation_(0) {}

TimerRegistry::~TimerRegistry() = default;

TimerHandle TimerRegistry::register_timer(std::string name) {
    if (name.empty()) {
        throw std::invalid_argument("timer name must not be empty");
    }

    std::lock_guard<std::mutex> lock(timers_mutex_);
    const auto existing = timers_.find(name);
    if (existing != timers_.end()) {
        return TimerHandle(this, existing->second.get());
    }

    auto state = std::make_unique<detail::TimerState>(std::move(name));
    auto* state_pointer = state.get();
    timers_.emplace(state_pointer->name(), std::move(state));
    return TimerHandle(this, state_pointer);
}

std::vector<TimerSnapshot> TimerRegistry::snapshots() const {
    std::vector<detail::TimerState*> states;
    {
        std::lock_guard<std::mutex> lock(timers_mutex_);
        states.reserve(timers_.size());
        for (const auto& entry : timers_) {
            states.push_back(entry.second.get());
        }
    }

    std::vector<TimerSnapshot> values;
    values.reserve(states.size());
    for (const auto* state : states) {
        values.push_back(state->snapshot());
    }
    std::sort(values.begin(), values.end(),
              [](const TimerSnapshot& left, const TimerSnapshot& right) {
                  return left.name() < right.name();
              });
    return values;
}

void TimerRegistry::reset_all() noexcept {
    std::lock_guard<std::mutex> lock(timers_mutex_);
    for (const auto& entry : timers_) {
        entry.second->reset();
    }
}

void TimerRegistry::set_enabled(bool enabled) noexcept {
    std::unique_lock<std::shared_mutex> lock(control_mutex_);
    if (enabled_ == enabled) {
        return;
    }
    enabled_ = enabled;
    ++generation_;
}

bool TimerRegistry::enabled() const noexcept {
    std::shared_lock<std::shared_mutex> lock(control_mutex_);
    return enabled_;
}

ManualTimer TimerRegistry::start(detail::TimerState* state) const noexcept {
    std::shared_lock<std::shared_mutex> lock(control_mutex_);
    if (!enabled_) {
        return ManualTimer();
    }
    const auto timer_generation = state->generation();
    const auto started_at = std::chrono::steady_clock::now();
    return ManualTimer(this, state, started_at, timer_generation, generation_);
}

bool TimerRegistry::observe(detail::TimerState* state,
                            std::chrono::nanoseconds duration) const noexcept {
    std::shared_lock<std::shared_mutex> lock(control_mutex_);
    if (!enabled_) {
        return false;
    }
    return state->observe_current(duration);
}

bool TimerRegistry::finish(detail::TimerState* state,
                           std::chrono::steady_clock::time_point started_at,
                           std::uint64_t timer_generation,
                           std::uint64_t registry_generation) const noexcept {
    const auto stopped_at = std::chrono::steady_clock::now();
    std::shared_lock<std::shared_mutex> lock(control_mutex_);
    if (!enabled_ || registry_generation != generation_) {
        return false;
    }
    const auto duration =
        std::chrono::duration_cast<std::chrono::nanoseconds>(stopped_at - started_at);
    return state->observe(duration, timer_generation);
}

}  // namespace vtb::instrumentation
