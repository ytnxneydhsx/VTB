#pragma once

#include <QWidget>

class QBoxLayout;
class QJsonObject;
class QLabel;

namespace vtb::desktop {

class MonitorIoGroup;

class MonitorIo final : public QWidget {
public:
    explicit MonitorIo(QWidget* parent = nullptr);

    auto addOverview(QBoxLayout& parent, QWidget* action) -> void;
    auto showSample(const QJsonObject& io) -> void;
    auto showAvailability(bool supported, bool stale) -> void;

private:
    auto updateOverview() -> void;

    MonitorIoGroup* network_ = nullptr;
    MonitorIoGroup* disks_ = nullptr;
    QLabel* overview_network_ = nullptr;
    QLabel* overview_disks_ = nullptr;
    bool supported_ = true;
    bool stale_ = false;
    qint64 time_ms_ = 0;
};

} // namespace vtb::desktop
