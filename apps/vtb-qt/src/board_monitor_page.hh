#pragma once

#include <QScrollArea>

#include <array>

class QJsonObject;
class QJsonArray;
class QLabel;
class QProgressBar;
class QTabBar;

namespace vtb::desktop {

enum class MonitorState;
class BoardStatus;
class MonitorIo;
class MonitorProcesses;

class BoardMonitorPage final : public QScrollArea {
public:
    explicit BoardMonitorPage(QWidget* parent = nullptr);

    auto showSnapshot(const QJsonObject& snapshot) -> void;
    auto showProcesses(const QJsonArray& rows, bool readable) -> void;
    auto showServiceStatus(const BoardStatus& status) -> void;
    auto showStreamState(MonitorState state, const QString& detail) -> void;

private:
    class CoreRow {
    public:
        QProgressBar* meter = nullptr;
        QLabel* load = nullptr;
        QLabel* frequency = nullptr;
    };

    auto updateStreamLabel() -> void;

    QTabBar* tabs_ = nullptr;
    MonitorProcesses* processes_ = nullptr;
    MonitorIo* io_ = nullptr;
    QLabel* stream_state_ = nullptr;
    QLabel* cpu_total_ = nullptr;
    QProgressBar* cpu_meter_ = nullptr;
    QLabel* memory_used_ = nullptr;
    QLabel* memory_total_ = nullptr;
    QLabel* memory_usage_ = nullptr;
    QProgressBar* memory_meter_ = nullptr;
    QLabel* soc_temperature_ = nullptr;
    QLabel* gpu_load_ = nullptr;
    QProgressBar* gpu_meter_ = nullptr;
    QLabel* gpu_frequency_ = nullptr;
    QLabel* npu_frequency_ = nullptr;
    std::array<CoreRow, 8> cores_ {};
    std::array<QLabel*, 6> temperatures_ {};
    QString last_update_;
    QString last_details_update_;
    MonitorState state_;
    bool details_supported_ = true;
};

} // namespace vtb::desktop
