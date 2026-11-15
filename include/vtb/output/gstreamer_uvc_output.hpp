#pragma once

#include <atomic>
#include <chrono>
#include <string>
#include <sys/types.h>
#include <vector>

#include "vtb/output/output.hpp"

namespace vtb {

class GstreamerUvcOutputConfig {
public:
    std::string gst_launch_path;
    std::string device_path;
    std::string library_path;
    std::string plugin_path;
    int poll_timeout_ms = 250;
    std::chrono::milliseconds stop_timeout{2000};
};

class GstreamerUvcOutput final : public Output {
public:
    explicit GstreamerUvcOutput(GstreamerUvcOutputConfig config);
    ~GstreamerUvcOutput() override;

    GstreamerUvcOutput(const GstreamerUvcOutput&) = delete;
    GstreamerUvcOutput& operator=(const GstreamerUvcOutput&) = delete;
    GstreamerUvcOutput(GstreamerUvcOutput&&) = delete;
    GstreamerUvcOutput& operator=(GstreamerUvcOutput&&) = delete;

    bool supports(const StreamDescriptor& descriptor) const override;
    void start(const std::vector<StreamDescriptor>& descriptors) override;
    void write(MediaPacket packet) override;
    void request_stop() noexcept override;
    void stop() noexcept override;
    std::string name() const override;

private:
    GstreamerUvcOutputConfig config_;
    int stream_fd_ = -1;
    int stop_fd_ = -1;
    pid_t child_pid_ = -1;
    std::atomic<bool> stop_requested_{false};
    std::string stream_id_;
};

}  // namespace vtb
