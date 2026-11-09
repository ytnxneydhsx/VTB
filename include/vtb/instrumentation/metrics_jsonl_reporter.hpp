#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include "vtb/instrumentation/timer.hpp"

namespace vtb::instrumentation {

class MetricsJsonlReporter final {
public:
    MetricsJsonlReporter(
        const TimerRegistry& registry,
        std::string source,
        std::string output_path,
        std::chrono::milliseconds report_interval);
    ~MetricsJsonlReporter();

    MetricsJsonlReporter(const MetricsJsonlReporter&) = delete;
    MetricsJsonlReporter& operator=(const MetricsJsonlReporter&) = delete;
    MetricsJsonlReporter(MetricsJsonlReporter&&) = delete;
    MetricsJsonlReporter& operator=(MetricsJsonlReporter&&) = delete;

    void start();
    bool write_now() noexcept;
    void stop() noexcept;

    bool started() const noexcept;
    bool failed() const noexcept;
    std::string last_error() const;
    std::uint64_t record_count() const noexcept;
    const std::string& source() const noexcept;
    const std::string& output_path() const noexcept;

private:
    void run() noexcept;
    bool write_snapshot(bool final_record) noexcept;
    void record_failure(const std::string& message) noexcept;

    const TimerRegistry& registry_;
    std::string source_;
    std::string output_path_;
    std::chrono::milliseconds report_interval_;
    std::ofstream output_;
    std::thread worker_;
    mutable std::mutex state_mutex_;
    std::mutex output_mutex_;
    std::condition_variable wakeup_;
    std::string last_error_;
    std::uint64_t record_count_ = 0;
    bool started_ = false;
    bool stop_requested_ = false;
    bool failed_ = false;
};

}  // namespace vtb::instrumentation
