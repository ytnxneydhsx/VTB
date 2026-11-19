#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <iosfwd>
#include <map>
#include <optional>
#include <string>

class MonitorDetails {
public:
    MonitorDetails();
    auto collect() -> nlohmann::json;
    auto writeProcesses(std::ostream& output, std::int64_t sample_time_ms) const -> void;

private:
    class ProcessTicks {
    public:
        std::uint64_t start_time = 0;
        std::uint64_t cpu = 0;
    };

    class IoCounters {
    public:
        std::string name;
        std::string state;
        std::uint64_t incoming = 0;
        std::uint64_t outgoing = 0;
    };

    using Devices = std::map<std::string, IoCounters>;

    static auto readNetwork() -> std::optional<Devices>;
    static auto readDisks() -> std::optional<Devices>;
    static auto deviceRates(const Devices& previous, const Devices& current,
        double seconds, double bytes_per_unit) -> nlohmann::json;
    auto collectProcesses(double seconds) -> void;
    auto username(const std::string& process_dir) -> std::string;

    const long clock_ticks_;
    const long page_size_;
    std::chrono::steady_clock::time_point last_sample_{};
    Devices previous_network_;
    Devices previous_disks_;
    std::map<int, ProcessTicks> previous_processes_;
    std::map<unsigned, std::string> usernames_;
    nlohmann::json rows_ = nlohmann::json::array();
};
