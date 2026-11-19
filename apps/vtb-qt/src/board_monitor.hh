#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QTimer>

namespace vtb::desktop {

enum class MonitorState { CONNECTING, LIVE, STALE };

class BoardMonitor final : public QObject {
    Q_OBJECT

public:
    explicit BoardMonitor(bool demo, QObject* parent = nullptr);
    ~BoardMonitor() override;

    BoardMonitor(const BoardMonitor&) = delete;
    auto operator=(const BoardMonitor&) -> BoardMonitor& = delete;
    BoardMonitor(BoardMonitor&&) = delete;
    auto operator=(BoardMonitor&&) -> BoardMonitor& = delete;

    auto start() -> void;
    auto setDemoMainPid(int pid) -> void;

Q_SIGNALS:
    void snapshotReceived(const QJsonObject& snapshot);
    void processesReceived(const QJsonArray& rows, bool readable);
    void stateChanged(MonitorState state, const QString& detail);

private:
    auto open() -> void;
    auto readSnapshots() -> void;
    auto readProcesses(const QJsonObject& chunk) -> bool;
    auto stopWithError(const QString& detail) -> void;
    auto finished() -> void;

    const bool demo_;
    QProcess ssh_;
    QTimer deadline_;
    QTimer next_;
    QByteArray pending_;
    QJsonArray process_rows_;
    qint64 process_time_ = 0;
    int process_total_ = -1;
    bool processes_readable_ = false;
    int demo_tick_ = 0;
    int demo_main_pid_ = 0;
    QString failure_;
};

} // namespace vtb::desktop
