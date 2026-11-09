#include "vtb/instrumentation/metrics_jsonl_reporter.hpp"

#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <unistd.h>

#include "vtb/time/monotonic_clock.hpp"

namespace vtb::instrumentation {
namespace {

std::uint64_t realtime_now_ns() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

std::string escape_json(const std::string& value) {
    std::ostringstream escaped;
    for (const unsigned char character : value) {
        switch (character) {
            case '\"':
                escaped << "\\\"";
                break;
            case '\\':
                escaped << "\\\\";
                break;
            case '\b':
                escaped << "\\b";
                break;
            case '\f':
                escaped << "\\f";
                break;
            case '\n':
                escaped << "\\n";
                break;
            case '\r':
                escaped << "\\r";
                break;
            case '\t':
                escaped << "\\t";
                break;
            default:
                if (character < 0x20U) {
                    constexpr char digits[] = "0123456789abcdef";
                    escaped << "\\u00"
                            << digits[(character >> 4U) & 0x0fU]
                            << digits[character & 0x0fU];
                } else {
                    escaped << static_cast<char>(character);
                }
                break;
        }
    }
    return escaped.str();
}

std::string serialize_record(
    const std::string& source,
    std::uint64_t sequence,
    bool final_record,
    const std::vector<TimerSnapshot>& snapshots) {
    std::ostringstream record;
    record << "{\"schema_version\":1"
           << ",\"source\":\"" << escape_json(source) << "\""
           << ",\"process_id\":" << static_cast<std::uint64_t>(getpid())
           << ",\"sequence\":" << sequence
           << ",\"recorded_monotonic_ns\":" << MonotonicClock::now_ns()
           << ",\"recorded_unix_ns\":" << realtime_now_ns()
           << ",\"final\":";
    if (final_record) {
        record << "true";
    } else {
        record << "false";
    }
    record << ",\"metrics\":[";
    bool first = true;
    for (const TimerSnapshot& snapshot : snapshots) {
        if (!first) {
            record << ',';
        }
        first = false;
        record << "{\"name\":\"" << escape_json(snapshot.name()) << "\""
               << ",\"sample_count\":" << snapshot.sample_count()
               << ",\"latest_ns\":" << snapshot.latest().count()
               << ",\"total_ns\":" << snapshot.total().count()
               << ",\"minimum_ns\":" << snapshot.minimum().count()
               << ",\"maximum_ns\":" << snapshot.maximum().count()
               << ",\"average_ns\":" << snapshot.average().count()
               << '}';
    }
    record << "]}\n";
    return record.str();
}

}  // namespace

MetricsJsonlReporter::MetricsJsonlReporter(
    const TimerRegistry& registry,
    std::string source,
    std::string output_path,
    std::chrono::milliseconds report_interval)
    : registry_(registry),
      source_(std::move(source)),
      output_path_(std::move(output_path)),
      report_interval_(report_interval) {
    if (source_.empty()) {
        throw std::invalid_argument("Metrics reporter source must not be empty");
    }
    if (output_path_.empty()) {
        throw std::invalid_argument("Metrics reporter output path must not be empty");
    }
    if (report_interval_.count() <= 0) {
        throw std::invalid_argument("Metrics reporter interval must be positive");
    }
}

MetricsJsonlReporter::~MetricsJsonlReporter() {
    stop();
}

void MetricsJsonlReporter::start() {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (started_) {
            throw std::logic_error("Metrics reporter can only be started once");
        }
    }

    const std::filesystem::path path(output_path_);
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }
    output_.open(output_path_, std::ios::out | std::ios::app);
    if (!output_.is_open()) {
        throw std::runtime_error(
            "Failed to open metrics JSONL file: " + output_path_);
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        started_ = true;
        stop_requested_ = false;
    }
    if (!write_snapshot(false)) {
        const std::string error = last_error();
        output_.close();
        std::lock_guard<std::mutex> lock(state_mutex_);
        started_ = false;
        throw std::runtime_error(error);
    }

    try {
        worker_ = std::thread(&MetricsJsonlReporter::run, this);
    } catch (...) {
        output_.close();
        std::lock_guard<std::mutex> lock(state_mutex_);
        started_ = false;
        throw;
    }
}

bool MetricsJsonlReporter::write_now() noexcept {
    return write_snapshot(false);
}

void MetricsJsonlReporter::stop() noexcept {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!started_) {
            return;
        }
        stop_requested_ = true;
    }
    wakeup_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    {
        std::lock_guard<std::mutex> lock(output_mutex_);
        output_.close();
    }
    std::lock_guard<std::mutex> lock(state_mutex_);
    started_ = false;
}

bool MetricsJsonlReporter::started() const noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return started_;
}

bool MetricsJsonlReporter::failed() const noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return failed_;
}

std::string MetricsJsonlReporter::last_error() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return last_error_;
}

std::uint64_t MetricsJsonlReporter::record_count() const noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return record_count_;
}

const std::string& MetricsJsonlReporter::source() const noexcept {
    return source_;
}

const std::string& MetricsJsonlReporter::output_path() const noexcept {
    return output_path_;
}

void MetricsJsonlReporter::run() noexcept {
    while (true) {
        std::unique_lock<std::mutex> lock(state_mutex_);
        const bool stopping = wakeup_.wait_for(
            lock,
            report_interval_,
            [this] { return stop_requested_; });
        lock.unlock();
        if (stopping) {
            break;
        }
        if (!write_snapshot(false)) {
            return;
        }
    }
    write_snapshot(true);
}

bool MetricsJsonlReporter::write_snapshot(bool final_record) noexcept {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!started_ || failed_ || (!final_record && stop_requested_)) {
            return false;
        }
    }

    try {
        const std::vector<TimerSnapshot> snapshots = registry_.snapshots();
        std::lock_guard<std::mutex> output_lock(output_mutex_);
        std::uint64_t sequence = 0;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (!started_ || failed_ || (!final_record && stop_requested_)) {
                return false;
            }
            sequence = record_count_;
        }
        const std::string record =
            serialize_record(source_, sequence, final_record, snapshots);
        output_ << record;
        output_.flush();
        if (!output_) {
            record_failure(
                "Failed to write metrics JSONL file: " + output_path_);
            return false;
        }
        std::lock_guard<std::mutex> lock(state_mutex_);
        ++record_count_;
        return true;
    } catch (const std::exception& error) {
        record_failure(error.what());
    } catch (...) {
        record_failure("Unknown metrics JSONL persistence failure");
    }
    return false;
}

void MetricsJsonlReporter::record_failure(const std::string& message) noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!failed_) {
        failed_ = true;
        last_error_ = message;
    }
}

}  // namespace vtb::instrumentation
