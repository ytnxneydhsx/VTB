#include "details.hh"

#include <algorithm>
#include <array>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>

#include <pwd.h>
#include <unistd.h>

namespace {

using Json = nlohmann::json;
namespace fs = std::filesystem;

auto node_exists(const fs::path& path) -> bool {
    std::error_code error;
    return fs::exists(path, error);
}

auto word(const fs::path& path) -> std::string {
    std::ifstream stream(path);
    std::string value;
    stream >> value;
    return value;
}

template <typename Number>
auto parse_number(std::string_view field, Number& value) -> bool {
    const auto end = field.data() + field.size();
    const auto parsed = std::from_chars(field.data(), end, value);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

auto rate(std::uint64_t before, std::uint64_t after, double seconds, double scale) -> Json {
    if (seconds > 0 && after >= before) {
        return static_cast<double>(after - before) * scale / seconds;
    }
    return nullptr;
}

} // namespace

MonitorDetails::MonitorDetails()
    : clock_ticks_(sysconf(_SC_CLK_TCK)), page_size_(sysconf(_SC_PAGESIZE)) {}

auto MonitorDetails::readNetwork() -> std::optional<Devices> {
    Devices devices;
    std::ifstream source("/proc/net/dev");
    if (!source) {
        return std::nullopt;
    }
    std::string line;
    while (std::getline(source, line)) {
        const auto first = line.find_first_not_of(" \t");
        const auto colon = line.find(':');
        if (first == std::string::npos || colon == std::string::npos || first >= colon) {
            continue;
        }
        const auto name = line.substr(first, colon - first);
        const auto path = fs::path("/sys/class/net") / name;
        if (!node_exists(path / "device") && !node_exists(path / "wireless")) {
            continue;
        }
        std::array<std::uint64_t, 9> counters{};
        std::istringstream fields(line.substr(colon + 1));
        for (auto& value : counters) {
            fields >> value;
        }
        const auto index = word(path / "ifindex");
        if (fields && !index.empty()) {
            devices.emplace(name + ":" + index,
                IoCounters{name, word(path / "operstate"), counters[0], counters[8]});
        }
    }
    return devices;
}

auto MonitorDetails::readDisks() -> std::optional<Devices> {
    Devices devices;
    std::ifstream source("/proc/diskstats");
    if (!source) {
        return std::nullopt;
    }
    std::string line;
    while (std::getline(source, line)) {
        std::istringstream fields(line);
        unsigned major = 0;
        unsigned minor = 0;
        std::string name;
        std::array<std::uint64_t, 7> counters{};
        if (!(fields >> major >> minor >> name)) {
            continue;
        }
        const bool mmc = name.starts_with("mmcblk");
        if (mmc && (name.find("boot") != std::string::npos || name.ends_with("rpmb"))) {
            continue;
        }
        // /sys/block exposes whole devices. The device link excludes RAM and loop devices.
        const auto path = fs::path("/sys/block") / name;
        if (!node_exists(path / "device")) {
            continue;
        }
        for (auto& value : counters) {
            fields >> value;
        }
        if (!fields) {
            continue;
        }
        std::string type = "block";
        if (mmc) {
            type = word(path / "device/type");
            if (type == "MMC") {
                type = "eMMC";
            }
        } else if (name.starts_with("nvme")) {
            type = "NVMe";
        }
        devices.emplace(name + ":" + std::to_string(major) + ":" + std::to_string(minor),
            IoCounters{name, type, counters[2], counters[6]});
    }
    return devices;
}

auto MonitorDetails::deviceRates(const Devices& previous, const Devices& current,
    double seconds, double bytes_per_unit) -> Json {
    Json rows = Json::array();
    for (const auto& [id, device] : current) {
        Json row{{"id", id}, {"name", device.name}, {"state", device.state},
            {"inBps", nullptr}, {"outBps", nullptr}};
        if (const auto before = previous.find(id); before != previous.end()) {
            row["inBps"] = rate(before->second.incoming, device.incoming, seconds, bytes_per_unit);
            row["outBps"] = rate(before->second.outgoing, device.outgoing, seconds, bytes_per_unit);
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

auto MonitorDetails::username(const std::string& process_dir) -> std::string {
    std::ifstream status(process_dir + "/status");
    std::string line;
    while (std::getline(status, line)) {
        if (!line.starts_with("Uid:")) {
            continue;
        }
        std::istringstream fields(line.substr(4));
        unsigned real_uid = 0;
        unsigned effective_uid = 0;
        if (!(fields >> real_uid >> effective_uid)) {
            break;
        }
        if (const auto cached = usernames_.find(effective_uid); cached != usernames_.end()) {
            return cached->second;
        }
        auto name = std::to_string(effective_uid);
        if (const auto* account = getpwuid(effective_uid); account && account->pw_name) {
            name = std::string_view(account->pw_name).substr(0, 64);
        }
        return usernames_.emplace(effective_uid, std::move(name)).first->second;
    }
    return {};
}

auto MonitorDetails::collectProcesses(double seconds) -> void {
    std::map<int, ProcessTicks> current;
    rows_ = Json::array();
    std::error_code error;
    fs::directory_iterator entry("/proc", error);
    if (error) {
        rows_ = nullptr;
        previous_processes_.clear();
        return;
    }
    for (; !error && entry != fs::directory_iterator{}; entry.increment(error)) {
        const auto& path = entry->path();
        int pid = 0;
        if (!parse_number(path.filename().string(), pid) || pid <= 0) {
            continue;
        }
        std::ifstream stat_file(path / "stat");
        std::string stat;
        std::getline(stat_file, stat, '\0');
        const auto first = stat.find('(');
        const auto last = stat.rfind(')');
        if (first == std::string::npos || last == std::string::npos || last <= first) {
            continue;
        }
        // comm can contain spaces, closing parentheses and newlines.
        const auto name = stat.substr(first + 1, std::min(last - first - 1, std::size_t{64}));
        std::array<std::string, 22> fields;
        std::istringstream values(stat.substr(last + 1));
        for (auto& field : fields) {
            values >> field;
        }
        std::uint64_t user = 0;
        std::uint64_t system = 0;
        ProcessTicks ticks;
        std::int64_t pages = 0;
        // Indices begin at field 3 (state) in /proc/<pid>/stat.
        if (!values || !parse_number(fields[11], user) || !parse_number(fields[12], system)
            || !parse_number(fields[19], ticks.start_time) || !parse_number(fields[21], pages)) {
            continue;
        }
        ticks.cpu = user + system;
        Json cpu = nullptr;
        if (const auto before = previous_processes_.find(pid);
            before != previous_processes_.end() && before->second.start_time == ticks.start_time
            && clock_ticks_ > 0) {
            cpu = rate(before->second.cpu, ticks.cpu, seconds, 100.0 / clock_ticks_);
        }
        Json rss = nullptr;
        if (pages >= 0 && page_size_ > 0) {
            rss = static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(page_size_);
        }
        rows_.push_back({{"pid", pid}, {"name", name}, {"cpuPct", cpu}, {"rssBytes", rss},
            {"user", username(path.string())}});
        current.emplace(pid, ticks);
    }
    if (error) {
        rows_ = nullptr;
        current.clear();
    }
    previous_processes_ = std::move(current);
}

auto MonitorDetails::collect() -> Json {
    using namespace std::chrono;
    const auto now = steady_clock::now();
    double seconds = 0;
    if (last_sample_ != steady_clock::time_point{}) {
        seconds = duration<double>(now - last_sample_).count();
    }
    last_sample_ = now;
    auto network = readNetwork();
    auto disks = readDisks();
    Json io{
        {"timeMs", duration_cast<milliseconds>(now.time_since_epoch()).count()},
        {"network", nullptr},
        {"disks", nullptr},
    };
    if (network) {
        io["network"] = deviceRates(previous_network_, *network, seconds, 1);
        previous_network_ = std::move(*network);
    } else {
        previous_network_.clear();
    }
    if (disks) {
        // diskstats sectors are always 512 bytes, including on 4 KiB disks.
        io["disks"] = deviceRates(previous_disks_, *disks, seconds, 512);
        previous_disks_ = std::move(*disks);
    } else {
        previous_disks_.clear();
    }
    collectProcesses(seconds);
    return io;
}

auto MonitorDetails::writeProcesses(std::ostream& output, std::int64_t sample_time_ms) const -> void {
    // The SSH receiver bounds individual lines to 16 KiB, including escaped text.
    constexpr std::size_t kRowsPerLine = 16;
    std::size_t offset = 0;
    do {
        Json chunk = Json::array();
        const auto end = std::min(rows_.size(), offset + kRowsPerLine);
        for (auto row = offset; row < end; ++row) {
            chunk.push_back(rows_[row]);
        }
        const Json message{{"version", 1}, {"kind", "processes"}, {"sampleTimeMs", sample_time_ms},
            {"offset", offset}, {"total", rows_.size()}, {"readable", rows_.is_array()},
            {"rows", std::move(chunk)}};
        output << message.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n';
        offset = end;
    } while (output && offset < rows_.size());
}
