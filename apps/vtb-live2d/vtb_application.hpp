#pragma once

#include <atomic>
#include <memory>

namespace vtb {

class VtbApplication final {
public:
    VtbApplication(int argc, char** argv);
    ~VtbApplication();

    VtbApplication(const VtbApplication&) = delete;
    VtbApplication& operator=(const VtbApplication&) = delete;
    VtbApplication(VtbApplication&&) = delete;
    VtbApplication& operator=(VtbApplication&&) = delete;

    int run(const std::atomic<bool>& stop_requested);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vtb
