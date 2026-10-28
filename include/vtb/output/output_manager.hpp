#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "vtb/output/output.hpp"

namespace vtb {

enum class OutputState {
    stopped,
    starting,
    running,
    reconnecting,
    failed,
};

class OutputOptions {
public:
    std::size_t queue_capacity = 8;
    // Zero retries means retry indefinitely with capped exponential backoff.
    std::uint32_t max_retries = 0;
    std::chrono::milliseconds retry_backoff{100};
    std::chrono::milliseconds max_retry_backoff{5000};
};

class OutputStatus {
public:
    std::string id;
    std::string name;
    std::vector<std::string> streams;
    OutputState state = OutputState::stopped;
    std::size_t queue_depth = 0;
    std::size_t queue_capacity = 0;
    std::uint64_t published = 0;
    std::uint64_t written = 0;
    std::uint64_t dropped = 0;
    std::uint64_t errors = 0;
    std::uint64_t retry_attempts = 0;
    std::uint64_t reconnects = 0;
    std::uint64_t last_success_ns = 0;
    std::string last_error;
};

class OutputManager {
public:
    OutputManager();
    ~OutputManager();

    OutputManager(const OutputManager&) = delete;
    OutputManager& operator=(const OutputManager&) = delete;
    OutputManager(OutputManager&&) = delete;
    OutputManager& operator=(OutputManager&&) = delete;

    void register_stream(const StreamDescriptor& descriptor);
    void add(std::string output_id, std::unique_ptr<Output> output,
             OutputOptions options = {});
    void bind(const std::string& output_id, const std::string& stream_id);
    void unbind(const std::string& output_id, const std::string& stream_id);
    void start(const std::string& output_id);
    void stop(const std::string& output_id);
    void remove(const std::string& output_id);
    std::size_t publish(const MediaPacket& packet);
    std::vector<OutputStatus> status() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vtb
