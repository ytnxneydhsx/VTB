#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "vtb/output/output.hpp"

namespace vtb {

class UvcOutputConfig {
public:
    std::string device_path;
    std::uint32_t buffer_count = 4;
    int poll_timeout_ms = 250;
};

class UvcOutput final : public Output {
public:
    explicit UvcOutput(UvcOutputConfig config);
    ~UvcOutput() override;

    UvcOutput(const UvcOutput&) = delete;
    UvcOutput& operator=(const UvcOutput&) = delete;
    UvcOutput(UvcOutput&&) = delete;
    UvcOutput& operator=(UvcOutput&&) = delete;

    bool supports(const StreamDescriptor& descriptor) const override;
    void start(const std::vector<StreamDescriptor>& descriptors) override;
    void write(MediaPacket packet) override;
    void request_stop() noexcept override;
    void stop() noexcept override;
    std::string name() const override;

private:
    UvcOutputConfig config_;
    int fd_ = -1;
    int stop_fd_ = -1;
    std::atomic<bool> stop_requested_{false};
    std::uint32_t pixel_format_ = 0;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t size_image_ = 0;
    bool streaming_ = false;
    std::vector<void*> mappings_;
    std::vector<std::size_t> mapping_lengths_;
    std::vector<bool> queued_;
    std::string stream_id_;
};

}  // namespace vtb
