#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "vtb/output/output.hpp"

struct AVFormatContext;

namespace vtb {

class RtspOutput final : public Output {
public:
    explicit RtspOutput(
        std::string url,
        std::chrono::milliseconds io_timeout = std::chrono::milliseconds{5000});
    ~RtspOutput() override;

    RtspOutput(const RtspOutput&) = delete;
    RtspOutput& operator=(const RtspOutput&) = delete;
    RtspOutput(RtspOutput&&) = delete;
    RtspOutput& operator=(RtspOutput&&) = delete;

    bool supports(const StreamDescriptor& descriptor) const override;
    void start(const std::vector<StreamDescriptor>& descriptors) override;
    void write(const MediaPacket& packet) override;
    void request_stop() noexcept override;
    void stop() noexcept override;
    std::string name() const override;

private:
    static int interrupt_callback(void* opaque) noexcept;
    void set_operation_deadline() noexcept;
    void release_context() noexcept;

    std::string url_;
    std::chrono::milliseconds io_timeout_;
    AVFormatContext* context_ = nullptr;
    std::map<std::string, int> stream_indexes_;
    std::thread::id owner_thread_;
    std::atomic<bool> cancelled_{false};
    std::atomic<std::int64_t> deadline_ns_{0};
    bool header_written_ = false;
};

}  // namespace vtb
