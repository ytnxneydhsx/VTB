#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

#include <unistd.h>

#include "vtb/instrumentation/metrics_jsonl_reporter.hpp"

namespace {

using namespace std::chrono_literals;

void require(bool condition) {
    if (!condition) {
        throw std::runtime_error("metrics JSONL reporter test check failed");
    }
}

void test_periodic_reporter_writes_jsonl_snapshots() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("vtb-metrics-jsonl-test-" + std::to_string(getpid()));
    const std::filesystem::path output_path = directory / "metrics.jsonl";
    std::filesystem::create_directories(directory);

    vtb::instrumentation::TimerRegistry registry;
    auto timer = registry.register_timer("test.metric");
    require(timer.observe(10ns));

    {
        vtb::instrumentation::MetricsJsonlReporter reporter(
            registry,
            "test-source",
            output_path.string(),
            std::chrono::hours(1));
        reporter.start();
        require(reporter.started());
        require(reporter.record_count() == 1);
        require(timer.observe(20ns));
        require(reporter.write_now());
        require(reporter.record_count() == 2);
        reporter.stop();
        require(!reporter.started());
        require(!reporter.failed());
        require(reporter.record_count() == 3);
    }

    std::ifstream input(output_path);
    require(input.is_open());
    std::string contents(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    require(contents.find("\"schema_version\":1") != std::string::npos);
    require(contents.find("\"source\":\"test-source\"") != std::string::npos);
    require(contents.find("\"name\":\"test.metric\"") != std::string::npos);
    require(contents.find("\"sample_count\":2") != std::string::npos);
    require(contents.find("\"average_ns\":15") != std::string::npos);
    require(contents.find("\"final\":true") != std::string::npos);

    input.close();
    std::filesystem::remove(output_path);
    std::filesystem::remove(directory);
}

void test_reporter_rejects_directory_as_output_file() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("vtb-metrics-jsonl-invalid-output-test-" + std::to_string(getpid()));
    std::filesystem::create_directories(directory);

    vtb::instrumentation::TimerRegistry registry;
    vtb::instrumentation::MetricsJsonlReporter reporter(
        registry,
        "test-source",
        directory.string(),
        std::chrono::hours(1));
    bool rejected = false;
    try {
        reporter.start();
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected);
    require(!reporter.started());

    std::filesystem::remove(directory);
}

}  // namespace

int main() {
    test_periodic_reporter_writes_jsonl_snapshots();
    test_reporter_rejects_directory_as_output_file();
    return 0;
}
