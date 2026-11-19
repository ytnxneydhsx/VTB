#pragma once

#include <QImage>
#include <QUrl>

#include <memory>

namespace vtb::desktop {

class WebRtcReceiver final {
public:
    WebRtcReceiver() = default;
    ~WebRtcReceiver();
    WebRtcReceiver(const WebRtcReceiver&) = delete;
    auto operator=(const WebRtcReceiver&) -> WebRtcReceiver& = delete;
    WebRtcReceiver(WebRtcReceiver&&) = delete;
    auto operator=(WebRtcReceiver&&) -> WebRtcReceiver& = delete;

    auto start(const QUrl& url) -> void;
    auto stop() -> void;
    auto takeFrame() -> QImage;
    auto error() -> QString;

private:
    class Session;
    std::shared_ptr<Session> session_;
    QString startup_error_;
};

} // namespace vtb::desktop
