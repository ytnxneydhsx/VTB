#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace vtb::instrumentation {

namespace detail {
class TimerState;
}

class TimerRegistry;
class ManualTimer;
class ScopedTimer;

class TimerSnapshot {
public:
    ~TimerSnapshot() = default;
    TimerSnapshot(const TimerSnapshot&) = default;
    TimerSnapshot& operator=(const TimerSnapshot&) = default;
    TimerSnapshot(TimerSnapshot&&) noexcept = default;
    TimerSnapshot& operator=(TimerSnapshot&&) noexcept = default;

    const std::string& name() const noexcept;
    bool has_samples() const noexcept;
    std::uint64_t sample_count() const noexcept;
    std::chrono::nanoseconds latest() const noexcept;
    std::chrono::nanoseconds total() const noexcept;
    std::chrono::nanoseconds minimum() const noexcept;
    std::chrono::nanoseconds maximum() const noexcept;
    std::chrono::nanoseconds average() const noexcept;

private:
    friend class detail::TimerState;

    TimerSnapshot(std::string name, std::uint64_t sample_count,
                  std::chrono::nanoseconds latest,
                  std::chrono::nanoseconds total,
                  std::chrono::nanoseconds minimum,
                  std::chrono::nanoseconds maximum,
                  std::chrono::nanoseconds average);

    std::string name_;
    std::uint64_t sample_count_;
    std::chrono::nanoseconds latest_;
    std::chrono::nanoseconds total_;
    std::chrono::nanoseconds minimum_;
    std::chrono::nanoseconds maximum_;
    std::chrono::nanoseconds average_;
};

class TimerHandle {
public:
    ~TimerHandle() = default;
    TimerHandle(const TimerHandle&) = default;
    TimerHandle& operator=(const TimerHandle&) = default;
    TimerHandle(TimerHandle&&) noexcept = default;
    TimerHandle& operator=(TimerHandle&&) noexcept = default;

    ManualTimer start() const noexcept;
    ScopedTimer scope() const noexcept;
    bool observe(std::chrono::nanoseconds duration) const noexcept;
    TimerSnapshot snapshot() const;
    void reset() const noexcept;
    const std::string& name() const noexcept;

private:
    friend class TimerRegistry;

    TimerHandle(TimerRegistry* registry, detail::TimerState* state) noexcept;

    TimerRegistry* registry_;
    detail::TimerState* state_;
};

class ManualTimer {
public:
    ~ManualTimer() = default;
    ManualTimer(const ManualTimer&) = delete;
    ManualTimer& operator=(const ManualTimer&) = delete;
    ManualTimer(ManualTimer&& other) noexcept;
    ManualTimer& operator=(ManualTimer&& other) noexcept;

    bool stop() noexcept;
    bool active() const noexcept;

private:
    friend class TimerRegistry;

    using Clock = std::chrono::steady_clock;

    ManualTimer() noexcept;
    ManualTimer(const TimerRegistry* registry, detail::TimerState* state,
                Clock::time_point started_at, std::uint64_t timer_generation,
                std::uint64_t registry_generation) noexcept;

    const TimerRegistry* registry_;
    detail::TimerState* state_;
    Clock::time_point started_at_;
    std::uint64_t timer_generation_;
    std::uint64_t registry_generation_;
    bool active_;
};

class ScopedTimer {
public:
    ~ScopedTimer();
    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;
    ScopedTimer(ScopedTimer&&) = delete;
    ScopedTimer& operator=(ScopedTimer&&) = delete;

private:
    friend class TimerHandle;

    explicit ScopedTimer(ManualTimer timer) noexcept;

    ManualTimer timer_;
};

class TimerRegistry {
public:
    TimerRegistry();
    ~TimerRegistry();
    TimerRegistry(const TimerRegistry&) = delete;
    TimerRegistry& operator=(const TimerRegistry&) = delete;
    TimerRegistry(TimerRegistry&&) = delete;
    TimerRegistry& operator=(TimerRegistry&&) = delete;

    TimerHandle register_timer(std::string name);
    std::vector<TimerSnapshot> snapshots() const;
    void reset_all() noexcept;
    void set_enabled(bool enabled) noexcept;
    bool enabled() const noexcept;

private:
    friend class TimerHandle;
    friend class ManualTimer;

    ManualTimer start(detail::TimerState* state) const noexcept;
    bool observe(detail::TimerState* state,
                 std::chrono::nanoseconds duration) const noexcept;
    bool finish(detail::TimerState* state,
                std::chrono::steady_clock::time_point started_at,
                std::uint64_t timer_generation,
                std::uint64_t registry_generation) const noexcept;

    mutable std::mutex timers_mutex_;
    std::unordered_map<std::string, std::unique_ptr<detail::TimerState>> timers_;
    mutable std::shared_mutex control_mutex_;
    bool enabled_;
    std::uint64_t generation_;
};

}  // namespace vtb::instrumentation
