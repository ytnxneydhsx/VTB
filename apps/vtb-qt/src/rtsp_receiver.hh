#pragma once

#include <QImage>
#include <QString>

#include <atomic>
#include <mutex>
#include <thread>

namespace vtb::desktop {

class RtspReceiver final {
public:
    RtspReceiver() = default;
    ~RtspReceiver();
    RtspReceiver(const RtspReceiver&) = delete;
    auto operator=(const RtspReceiver&) -> RtspReceiver& = delete;
    RtspReceiver(RtspReceiver&&) = delete;
    auto operator=(RtspReceiver&&) -> RtspReceiver& = delete;

    auto start(const QString& url) -> void;
    auto stop() -> void;
    auto takeFrame() -> QImage;
    auto error() -> QString;

private:
    auto receive(const QByteArray& url) -> void;
    auto fail(int result) -> void;

    std::atomic<bool> cancelled_{ false };
    std::mutex mutex_;
    QImage latest_;
    QString error_;
    std::thread worker_;
};

} // namespace vtb::desktop
