#pragma once

#include <memory>
#include "vtb/config/vtb_application_config.hpp"

namespace vtb {
class Live2DTrackingSession;

class Live2DControlServer final {
public:
    // session must outlive this server; stop() joins all control requests.
    Live2DControlServer(ControlConfiguration config, Live2DTrackingSession& session);
    ~Live2DControlServer();
    Live2DControlServer(const Live2DControlServer&) = delete;
    Live2DControlServer& operator=(const Live2DControlServer&) = delete;
    Live2DControlServer(Live2DControlServer&&) = delete;
    Live2DControlServer& operator=(Live2DControlServer&&) = delete;

    void start();
    void request_stop() noexcept;
    void stop() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace vtb
