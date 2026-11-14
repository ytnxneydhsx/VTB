#pragma once

#include <memory>
#include "vtb/config/vtb_application_config.hpp"

namespace vtb {
class FaceTrackingStage;

class Live2DControlServer final {
public:
    // tracking_stage must outlive this server; stop() joins all requests.
    Live2DControlServer(
        ControlConfiguration config,
        FaceTrackingStage& tracking_stage);
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
