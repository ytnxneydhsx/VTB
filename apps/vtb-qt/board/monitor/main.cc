#include <nlohmann/json.hpp>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

#include <poll.h>
#include <unistd.h>

namespace {

using Json = nlohmann::json;
namespace fs = std::filesystem;

class CpuTicks {
public:
    std::uint64_t total = 0;
    std::uint64_t idle = 0;
};

using CpuTimes = std::map<int, CpuTicks>;
constexpr int kTotalCpu = -1;

template <typename Number>
auto file_number(const fs::path& path) -> std::optional<Number> {
    std::ifstream stream(path);
    Number value{};
    if (stream >> value) {
        return value;
    }
    return std::nullopt;
}

auto read_cpu_times() -> CpuTimes {
    std::ifstream stream("/proc/stat");
    CpuTimes times;
    std::string line;
    while (std::getline(stream, line) && line.starts_with("cpu")) {
        std::istringstream fields(line);
        std::string name;
        fields >> name;
        int cpu = kTotalCpu;
        if (name != "cpu") {
            const auto digits = std::string_view(name).substr(3);
            const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), cpu);
            if (result.ec != std::errc{} || result.ptr != digits.data() + digits.size() || cpu < 0) {
                continue;
            }
        }
        CpuTicks ticks;
        // guest and guest_nice are already included in user and nice.
        for (int field = 0; field < 8; ++field) {
            std::uint64_t value = 0;
            if (!(fields >> value)) {
                break;
            }
            ticks.total += value;
            if (field == 3 || field == 4) {
                ticks.idle += value;
            }
        }
        if (ticks.total > 0) {
            times.emplace(cpu, ticks);
        }
    }
    return times;
}

auto usage(const CpuTimes& previous, int cpu, const CpuTicks& current) -> Json {
    const auto before = previous.find(cpu);
    if (before == previous.end() || current.total <= before->second.total
        || current.idle < before->second.idle) {
        return nullptr;
    }
    const auto elapsed = current.total - before->second.total;
    const auto idle = current.idle - before->second.idle;
    if (idle > elapsed) {
        return nullptr;
    }
    return 100.0 * static_cast<double>(elapsed - idle) / static_cast<double>(elapsed);
}

auto read_cpu_frequencies() -> std::map<int, std::uint64_t> {
    std::map<int, std::uint64_t> frequencies;
    std::error_code error;
    fs::directory_iterator entry("/sys/devices/system/cpu/cpufreq", error);
    for (; !error && entry != fs::directory_iterator{}; entry.increment(error)) {
        const auto& path = entry->path();
        if (!path.filename().string().starts_with("policy")) {
            continue;
        }
        const auto khz = file_number<std::uint64_t>(path / "scaling_cur_freq");
        if (!khz || *khz == 0) {
            continue;
        }
        std::ifstream members(path / "affected_cpus");
        int cpu = 0;
        while (members >> cpu) {
            frequencies[cpu] = *khz;
        }
    }
    return frequencies;
}

auto sample_cpu(const CpuTimes& previous, const CpuTimes& current) -> Json {
    const auto frequencies = read_cpu_frequencies();
    Json cores = Json::array();
    Json total = nullptr;
    for (const auto& [cpu, ticks] : current) {
        if (cpu == kTotalCpu) {
            total = usage(previous, cpu, ticks);
            continue;
        }
        Json core{{"id", cpu}, {"usagePct", usage(previous, cpu, ticks)}, {"frequencyKHz", nullptr}};
        if (const auto freq = frequencies.find(cpu); freq != frequencies.end()) {
            core["frequencyKHz"] = freq->second;
        }
        cores.push_back(std::move(core));
    }
    return {{"usagePct", total}, {"cores", std::move(cores)}};
}

auto sample_memory() -> Json {
    std::ifstream stream("/proc/meminfo");
    std::optional<std::uint64_t> total;
    std::optional<std::uint64_t> available;
    std::string line;
    while (std::getline(stream, line) && (!total || !available)) {
        std::istringstream fields(line);
        std::string key;
        std::uint64_t value = 0;
        if (!(fields >> key >> value)) {
            continue;
        }
        if (key == "MemTotal:") {
            total = value;
        } else if (key == "MemAvailable:") {
            available = value;
        }
    }
    if (!total || !available || *total == 0 || *available > *total) {
        return nullptr;
    }
    return {{"totalBytes", *total * 1024}, {"usedBytes", (*total - *available) * 1024}};
}

auto sample_temperatures() -> Json {
    Json values = Json::object();
    std::error_code error;
    fs::directory_iterator entry("/sys/class/thermal", error);
    for (; !error && entry != fs::directory_iterator{}; entry.increment(error)) {
        const auto& path = entry->path();
        if (!path.filename().string().starts_with("thermal_zone")) {
            continue;
        }
        std::ifstream type_file(path / "type");
        std::string type;
        const auto milli_c = file_number<std::int64_t>(path / "temp");
        if (milli_c && std::getline(type_file, type) && !type.empty()) {
            values[type] = *milli_c;
        }
    }
    return values;
}

auto frequency_hz(const fs::path& devfreq) -> Json {
    const auto hz = file_number<std::uint64_t>(devfreq / "cur_freq");
    if (hz && *hz > 0) {
        return *hz;
    }
    return nullptr;
}

auto gpu_load(const fs::path& devfreq) -> Json {
    std::ifstream stream(devfreq / "load");
    std::string value;
    if (stream >> value) {
        const auto separator = value.find('@');
        if (separator != std::string::npos) {
            unsigned load = 0;
            const auto result = std::from_chars(value.data(), value.data() + separator, load);
            if (result.ec == std::errc{} && result.ptr == value.data() + separator && load <= 100) {
                return load;
            }
        }
    }
    return nullptr;
}

auto sample(const CpuTimes& previous, const CpuTimes& current) -> Json {
    using namespace std::chrono;
    const auto time_ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    const fs::path gpu("/sys/class/devfreq/fb000000.gpu");
    const fs::path npu("/sys/class/devfreq/fdab0000.npu");
    return {
        {"version", 1},
        {"sampleTimeMs", time_ms},
        {"cpu", sample_cpu(previous, current)},
        {"memory", sample_memory()},
        {"temperatureMilliC", sample_temperatures()},
        {"gpu", {{"frequencyHz", frequency_hz(gpu)}, {"usagePct", gpu_load(gpu)}}},
        {"npu", {{"frequencyHz", frequency_hz(npu)}}},
    };
}

} // namespace

int main() {
    CpuTimes previous;
    for (;;) {
        auto current = read_cpu_times();
        std::cout << sample(previous, current).dump() << '\n' << std::flush;
        if (!std::cout) {
            return 1;
        }
        previous = std::move(current);

        // The SSH client's open input channel owns this sampling session.
        pollfd input{STDIN_FILENO, POLLIN, 0};
        const int ready = poll(&input, 1, 1000);
        if (ready > 0) {
            return 0;
        }
        if (ready < 0) {
            return 1;
        }
    }
}
