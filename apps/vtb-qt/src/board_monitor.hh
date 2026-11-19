#pragma once

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

Q_SIGNALS:
    void snapshotReceived(const QJsonObject& snapshot);
    void stateChanged(MonitorState state, const QString& detail);

private:
    auto open() -> void;
    auto readSnapshots() -> void;
    auto stopWithError(const QString& detail) -> void;
    auto finished() -> void;

    const bool demo_;
    QProcess ssh_;
    QTimer deadline_;
    QTimer next_;
    QByteArray pending_;
    QString failure_;
};

} // namespace vtb::desktop
