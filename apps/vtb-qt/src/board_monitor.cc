#include "board_monitor.hh"

#include "board_ssh.hh"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>

namespace vtb::desktop {
namespace {

    constexpr int kStartupDeadlineMs = 8000;
    constexpr int kSampleDeadlineMs = 4500;
    constexpr int kStopDeadlineMs = 1500;
    constexpr int kRetryDelayMs = 2000;
    constexpr qsizetype kMaxLineBytes = 16 * 1024;

    auto demo_snapshot() -> QJsonObject {
        QJsonArray cores;
        const int usages[] { 19, 41, 32, 72, 59, 13, 9, 11 };
        for (int id = 0; id < 8; ++id) {
            int frequency = 1800000;
            if (id == 3 || id == 4) {
                frequency = 2208000;
            }
            cores.append(QJsonObject{
                { "id", id }, { "usagePct", usages[id] }, { "frequencyKHz", frequency }
            });
        }
        return {
            { "version", 1 },
            { "sampleTimeMs", QDateTime::currentMSecsSinceEpoch() },
            { "cpu", QJsonObject{ { "usagePct", 32 }, { "cores", cores } } },
            { "memory", QJsonObject{ { "totalBytes", qint64{8019402752} },
                            { "usedBytes", qint64{1363652116} } } },
            { "temperatureMilliC",
                QJsonObject{ { "soc-thermal", 47200 }, { "bigcore0-thermal", 47200 },
                    { "bigcore1-thermal", 48100 }, { "littlecore-thermal", 48100 },
                    { "gpu-thermal", 46200 }, { "npu-thermal", 47200 },
                    { "center-thermal", 47200 } } },
            { "gpu", QJsonObject{ { "frequencyHz", 900000000 }, { "usagePct", 26 } } },
            { "npu", QJsonObject{ { "frequencyHz", 1000000000 } } },
        };
    }

} // namespace

BoardMonitor::BoardMonitor(bool demo, QObject* parent)
    : QObject(parent)
    , demo_(demo) {
    deadline_.setSingleShot(true);
    next_.setSingleShot(true);
    connect(&next_, &QTimer::timeout, this, &BoardMonitor::open);
    connect(&deadline_, &QTimer::timeout, this, [this] {
        stopWithError(QStringLiteral("等待板端监控数据超时。"));
    });
    connect(&ssh_, &QProcess::readyReadStandardOutput, this, &BoardMonitor::readSnapshots);
    connect(&ssh_, &QProcess::finished, this, &BoardMonitor::finished);
    connect(&ssh_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            failure_ = ssh_.errorString();
            finished();
        }
    });
}

BoardMonitor::~BoardMonitor() {
    deadline_.stop();
    next_.stop();
    ssh_.disconnect(this);
    if (ssh_.state() != QProcess::NotRunning) {
        ssh_.closeWriteChannel();
        if (!ssh_.waitForFinished(kStopDeadlineMs)) {
            ssh_.kill();
            ssh_.waitForFinished(1000);
        }
    }
}

auto BoardMonitor::start() -> void {
    Q_EMIT stateChanged(MonitorState::CONNECTING, {});
    open();
}

auto BoardMonitor::open() -> void {
    if (ssh_.state() != QProcess::NotRunning) {
        return;
    }
    if (demo_) {
        Q_EMIT snapshotReceived(demo_snapshot());
        Q_EMIT stateChanged(MonitorState::LIVE, {});
        next_.start(1000);
        return;
    }
    pending_.clear();
    failure_.clear();
    auto args = board_ssh_arguments();
    args << QStringLiteral("exec \"$HOME/VTB/runtime/vtb-board-monitor\"");
    deadline_.start(kStartupDeadlineMs);
    ssh_.start(QStringLiteral("/usr/bin/ssh"), args);
}

auto BoardMonitor::readSnapshots() -> void {
    auto bytes = ssh_.readAllStandardOutput();
    if (!failure_.isEmpty()) {
        return;
    }
    pending_.append(bytes);
    qsizetype end = pending_.indexOf('\n');
    while (end >= 0 && end <= kMaxLineBytes) {
        const auto value = QJsonDocument::fromJson(pending_.left(end)).object();
        pending_.remove(0, end + 1);
        if (value.value(QStringLiteral("version")).toInt() != 1
            || value.value(QStringLiteral("sampleTimeMs")).toInteger() <= 0
            || !value.value(QStringLiteral("cpu")).isObject()) {
            stopWithError(QStringLiteral("板端返回了无法识别的监控数据。"));
            return;
        }
        deadline_.start(kSampleDeadlineMs);
        Q_EMIT snapshotReceived(value);
        Q_EMIT stateChanged(MonitorState::LIVE, {});
        end = pending_.indexOf('\n');
    }
    if (pending_.size() > kMaxLineBytes) {
        stopWithError(QStringLiteral("板端监控数据超过单行长度限制。"));
    }
}

auto BoardMonitor::stopWithError(const QString& detail) -> void {
    failure_ = detail;
    ssh_.kill();
}

auto BoardMonitor::finished() -> void {
    deadline_.stop();
    pending_.clear();
    const auto stderr_text = QString::fromUtf8(ssh_.readAllStandardError().right(2048)).trimmed();
    if (failure_.isEmpty()) {
        failure_ = stderr_text;
    }
    if (failure_.isEmpty()) {
        failure_ = QStringLiteral("板端监控连接已结束。");
    }
    Q_EMIT stateChanged(MonitorState::STALE, failure_);
    next_.start(kRetryDelayMs);
}

} // namespace vtb::desktop
