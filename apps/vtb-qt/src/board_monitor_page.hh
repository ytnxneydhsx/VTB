#pragma once

#include <QScrollArea>

#include <array>

class QJsonObject;
class QLabel;
class QProgressBar;

namespace vtb::desktop {

enum class MonitorState;

class BoardMonitorPage final : public QScrollArea {
public:
    explicit BoardMonitorPage(QWidget* parent = nullptr);

    auto showSnapshot(const QJsonObject& snapshot) -> void;
    auto showStreamState(MonitorState state, const QString& detail) -> void;

private:
    class CoreRow {
    public:
        QProgressBar* meter = nullptr;
        QLabel* load = nullptr;
        QLabel* frequency = nullptr;
    };

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
};

} // namespace vtb::desktop
