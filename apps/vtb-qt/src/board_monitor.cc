#include "board_monitor.hh"

#include "board_ssh.hh"

#include <QDateTime>
#include <QJsonDocument>

namespace vtb::desktop {
namespace {

    constexpr int kStartupDeadlineMs = 8000;
    constexpr int kSampleDeadlineMs = 4500;
    constexpr int kStopDeadlineMs = 1500;
    constexpr int kRetryDelayMs = 2000;
    constexpr qsizetype kMaxLineBytes = 16 * 1024;

    auto demo_snapshot(int tick) -> QJsonObject {
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
        QJsonObject snapshot{
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
            { "details", true },
        };
        if (tick % 2 == 0) {
            const auto device = [](const char* name, const char* state, int in, int out) {
                return QJsonObject{{"id", name}, {"name", name}, {"state", state},
                    {"inBps", in}, {"outBps", out}};
            };
            snapshot["io"] = QJsonObject{{"timeMs", 1000LL * (tick + 1)},
                {"network", QJsonArray{device("end1", "up", 65536 + 521 * (tick % 18),
                    1064000 + 26121 * (tick % 24)), device("usb0", "down", 0, 0),
                    device("wlan0", "dormant", 0, 0)}},
                {"disks", QJsonArray{device("mmcblk0", "eMMC",
                    524288 + 124321 * (tick % 12), 102400 + 16471 * (tick % 8))}}};
        }
        return snapshot;
    }

    auto demo_processes(int tick, int main_pid) -> QJsonArray {
        const auto row = [](int pid, const char* name, double cpu, int kib, const char* user) {
            return QJsonObject{{"pid", pid}, {"name", name}, {"cpuPct", cpu},
                {"rssBytes", 1024 * kib}, {"user", user}};
        };
        QJsonArray rows{row(527, "mediamtx", 4.5, 18979, "root"),
            row(1, "systemd", 0.5, 13412, "root"),
            row(2682, "sshd", 0.1, 6100, "huangnv"), row(2, "kthreadd", 0, 0, "root")};
        if (main_pid > 0) {
            rows.append(row(main_pid, "vtb-live2d", 121.5 + tick % 12, 741384, "root"));
        }
        return rows;
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

auto BoardMonitor::setDemoMainPid(int pid) -> void { demo_main_pid_ = pid; }

auto BoardMonitor::open() -> void {
    if (ssh_.state() != QProcess::NotRunning) {
        return;
    }
    if (demo_) {
        Q_EMIT snapshotReceived(demo_snapshot(demo_tick_));
        if (demo_tick_ % 2 == 0) {
            Q_EMIT processesReceived(demo_processes(demo_tick_, demo_main_pid_), true);
        }
        ++demo_tick_;
        Q_EMIT stateChanged(MonitorState::LIVE, {});
        next_.start(1000);
        return;
    }
    pending_.clear();
    process_rows_ = {};
    process_time_ = 0;
    process_total_ = -1;
    failure_.clear();
    auto args = board_ssh_arguments();
    args << QStringLiteral("exec \"$HOME/VTB/runtime/vtb-board-monitor\" --details");
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
        const auto sample_time = value.value(QStringLiteral("sampleTimeMs")).toInteger();
        if (value.value(QStringLiteral("version")).toInt() != 1 || sample_time <= 0) {
            stopWithError(QStringLiteral("板端返回了无法识别的监控数据。"));
            return;
        }
        if (value.value(QStringLiteral("cpu")).isObject()) {
            if (process_time_ > 0) {
                stopWithError(QStringLiteral("板端进程快照不完整。"));
                return;
            }
            if (value.value(QStringLiteral("io")).isObject()) {
                process_time_ = sample_time;
            }
            deadline_.start(kSampleDeadlineMs);
            Q_EMIT snapshotReceived(value);
            Q_EMIT stateChanged(MonitorState::LIVE, {});
        } else if (value.value(QStringLiteral("kind")).toString() != QStringLiteral("processes")
            || !readProcesses(value)) {
            stopWithError(QStringLiteral("板端返回了无法识别的进程数据。"));
            return;
        }
        end = pending_.indexOf('\n');
    }
    if (pending_.size() > kMaxLineBytes) {
        stopWithError(QStringLiteral("板端监控数据超过单行长度限制。"));
    }
}

auto BoardMonitor::readProcesses(const QJsonObject& chunk) -> bool {
    const auto offset = chunk.value(QStringLiteral("offset")).toInt(-1);
    const auto total = chunk.value(QStringLiteral("total")).toInt(-1);
    const auto rows_value = chunk.value(QStringLiteral("rows"));
    const auto readable = chunk.value(QStringLiteral("readable"));
    if (process_time_ <= 0 || chunk.value(QStringLiteral("sampleTimeMs")).toInteger() != process_time_
        || offset != process_rows_.size() || offset < 0 || total < offset
        || !rows_value.isArray() || !readable.isBool()) {
        return false;
    }
    const auto rows = rows_value.toArray();
    if (rows.size() > total - offset || (rows.isEmpty() && total != 0)
        || (!readable.toBool() && total != 0)) {
        return false;
    }
    if (offset == 0) {
        process_total_ = total;
        processes_readable_ = readable.toBool();
    } else if (process_total_ != total || processes_readable_ != readable.toBool()) {
        return false;
    }
    for (const auto& row : rows) {
        process_rows_.append(row);
    }
    if (process_rows_.size() == process_total_) {
        Q_EMIT processesReceived(process_rows_, processes_readable_);
        process_rows_ = {};
        process_time_ = 0;
        process_total_ = -1;
    }
    return true;
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
