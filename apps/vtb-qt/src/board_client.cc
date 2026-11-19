#include "board_client.hh"

#include "board_ssh.hh"

#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>

namespace vtb::desktop {

namespace {

    constexpr auto kPollIntervalMs = 2600;
    constexpr auto kReadDeadlineMs = 18000;
    constexpr auto kWriteDeadlineMs = 94000;

    auto operation_name(Operation operation) -> QString {
        switch (operation) {
        case Operation::STATUS:
            return QStringLiteral("status");
        case Operation::START:
            return QStringLiteral("start");
        case Operation::STOP:
            return QStringLiteral("stop");
        case Operation::SAVE:
            return QStringLiteral("save");
        case Operation::APPLY:
            return QStringLiteral("apply");
        case Operation::LOGS:
            return QStringLiteral("logs");
        }
        Q_UNREACHABLE();
    }

} // namespace

auto BoardStatus::running() const -> bool { return service_state == QStringLiteral("active"); }

auto BoardStatus::changing() const -> bool {
    return service_state == QStringLiteral("activating")
        || service_state == QStringLiteral("deactivating")
        || service_state == QStringLiteral("reloading");
}

auto BoardStatus::configAvailable() const -> bool {
    return config_error.isEmpty() && !revision.isEmpty();
}

auto BoardStatus::webRtcPreviewAvailable() const -> bool {
    const QHostAddress address(bind_address);
    return connected && running() && webrtc_enabled && signalling_listening
        && !address.isNull() && !address.isLoopback() && !address.isLinkLocal();
}

BoardClient::BoardClient(bool demo, QObject* parent)
    : QObject(parent)
    , demo_(demo) {
    QFile script(QStringLiteral(":/vtb/control.py"));
    if (script.open(QIODevice::ReadOnly)) {
        script_ = script.readAll();
    }
    interval_.setInterval(kPollIntervalMs);
    deadline_.setSingleShot(true);
    connect(&interval_, &QTimer::timeout, this, &BoardClient::refresh);
    connect(&deadline_, &QTimer::timeout, this, [this] {
        timed_out_ = true;
        ssh_.kill();
    });
    connect(&ssh_, &QProcess::started, this, [this] {
        ssh_.write(script_);
        ssh_.closeWriteChannel();
    });
    connect(&ssh_, &QProcess::finished, this, &BoardClient::finish);
    connect(&ssh_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            complete({ }, ssh_.errorString());
        }
    });
}

BoardClient::~BoardClient() {
    ssh_.disconnect(this);
    if (ssh_.state() != QProcess::NotRunning) {
        ssh_.kill();
        ssh_.waitForFinished(1000);
    }
}

auto BoardClient::startMonitoring() -> void {
    if (demo_) {
        status_.connected = true;
        status_.load_state = QStringLiteral("loaded");
        status_.service_state = QStringLiteral("inactive");
        status_.service_result = QStringLiteral("success");
        status_.revision = QStringLiteral("demo");
        status_.bind_address = QStringLiteral("192.168.77.2");
        status_.rtsp_url = QStringLiteral("rtsp://192.168.77.2:8554/live2d");
        status_.uvc_enabled = true;
        status_.rtsp_enabled = true;
        Q_EMIT statusChanged(status_);
        return;
    }
    interval_.start();
    refresh();
}

auto BoardClient::refresh() -> void {
    if (!demo_ && !in_flight_) {
        dispatch({ Operation::STATUS, { } });
    }
}

auto BoardClient::operate(Operation operation, const QJsonObject& payload) -> void {
    if (busy_ || operation == Operation::STATUS || !status_.connected) {
        return;
    }
    busy_ = true;
    Q_EMIT busyChanged(true);
    if (demo_) {
        simulate({ operation, payload });
        return;
    }
    if (in_flight_) {
        waiting_ = Request { operation, payload };
        return;
    }
    dispatch({ operation, payload });
}

auto BoardClient::dispatch(const Request& request) -> void {
    active_ = request.operation;
    in_flight_ = true;
    timed_out_ = false;
    if (script_.isEmpty()) {
        in_flight_ = false;
        busy_ = false;
        Q_EMIT connectionError(QStringLiteral("找不到内置板端控制脚本"));
        Q_EMIT busyChanged(false);
        return;
    }

    auto args = board_ssh_arguments();
    if (request.operation != Operation::STATUS) {
        args << QStringLiteral("sudo") << QStringLiteral("-n") << QStringLiteral("--");
    }
    args << QStringLiteral("/usr/bin/python3") << QStringLiteral("-")
         << operation_name(request.operation);
    if (!request.payload.isEmpty()) {
        const auto bytes = QJsonDocument(request.payload).toJson(QJsonDocument::Compact);
        args << QString::fromLatin1(
            bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    }

    int deadline = kWriteDeadlineMs;
    if (request.operation == Operation::STATUS || request.operation == Operation::LOGS) {
        deadline = kReadDeadlineMs;
    }
    deadline_.start(deadline);
    ssh_.start(QStringLiteral("/usr/bin/ssh"), args);
}

auto BoardClient::finish(int exit_code, QProcess::ExitStatus exit_status) -> void {
    const auto output = ssh_.readAllStandardOutput();
    const auto error = QString::fromUtf8(ssh_.readAllStandardError()).trimmed();
    if (!in_flight_) {
        return;
    }
    if (timed_out_) {
        complete({ }, QStringLiteral("与板端通信超时，请回读状态确认结果。"));
        return;
    }
    if (exit_status != QProcess::NormalExit || exit_code != 0) {
        QString detail = error;
        if (detail.isEmpty()) {
            detail = QStringLiteral("SSH 操作失败（%1）").arg(exit_code);
        }
        complete({ }, detail);
        return;
    }
    const auto result = QJsonDocument::fromJson(output);
    if (!result.isObject() || !result.object().value(QStringLiteral("ok")).isBool()) {
        complete({ }, QStringLiteral("板端返回了无法识别的响应。"));
        return;
    }
    complete(result.object());
}

auto BoardClient::complete(const QJsonObject& reply, const QString& transport_error) -> void {
    deadline_.stop();
    in_flight_ = false;
    const bool ok = transport_error.isEmpty() && reply.value(QStringLiteral("ok")).toBool();
    QString detail = transport_error;
    if (detail.isEmpty() && !ok) {
        detail = reply.value(QStringLiteral("error")).toString();
    }
    if (active_ == Operation::STATUS) {
        if (ok) {
            publish(reply.value(QStringLiteral("status")).toObject());
        } else {
            status_.connected = false;
            Q_EMIT statusChanged(status_);
            Q_EMIT connectionError(detail);
        }
        if (waiting_) {
            const auto next = *waiting_;
            waiting_.reset();
            if (ok) {
                dispatch(next);
                return;
            }
            Q_EMIT operationFinished(next.operation, false, detail);
        }
        if (busy_) {
            busy_ = false;
            Q_EMIT busyChanged(false);
        }
        return;
    }
    if (ok && active_ == Operation::LOGS) {
        detail = reply.value(QStringLiteral("logs")).toString();
    }
    Q_EMIT operationFinished(active_, ok, detail);
    refresh();
}

auto BoardClient::publish(const QJsonObject& value) -> void {
    status_.connected = true;
    status_.load_state = value.value(QStringLiteral("loadState")).toString();
    status_.service_state = value.value(QStringLiteral("activeState")).toString();
    status_.service_result = value.value(QStringLiteral("serviceResult")).toString();
    status_.main_pid = value.value(QStringLiteral("mainPid")).toInt();
    status_.revision = value.value(QStringLiteral("revision")).toString();
    status_.config_error = value.value(QStringLiteral("configError")).toString();
    status_.bind_address = value.value(QStringLiteral("bindAddress")).toString();
    status_.rtsp_url = value.value(QStringLiteral("rtspUrl")).toString();
    status_.uvc_host_state = value.value(QStringLiteral("uvcHostState")).toString();
    status_.uvc_enabled = value.value(QStringLiteral("uvcEnabled")).toBool();
    status_.uvc_node_ready = value.value(QStringLiteral("uvcNodeReady")).toBool();
    status_.rtsp_enabled = value.value(QStringLiteral("rtspEnabled")).toBool();
    status_.rtsp_listening = value.value(QStringLiteral("rtspListening")).toBool();
    status_.webrtc_enabled = value.value(QStringLiteral("webrtcEnabled")).toBool();
    status_.signalling_listening = value.value(QStringLiteral("signallingListening")).toBool();
    status_.web_port = value.value(QStringLiteral("webPort")).toInt(8080);
    status_.signalling_port = value.value(QStringLiteral("signallingPort")).toInt(8443);
    Q_EMIT statusChanged(status_);
}

auto BoardClient::simulate(const Request& request) -> void {
    if (request.operation == Operation::START || request.operation == Operation::APPLY) {
        status_.service_state = QStringLiteral("activating");
    }
    if (request.operation == Operation::STOP) {
        status_.service_state = QStringLiteral("deactivating");
    }
    Q_EMIT statusChanged(status_);
    QTimer::singleShot(480, this, [this, request] {
        QString detail;
        switch (request.operation) {
        case Operation::START:
        case Operation::APPLY:
            status_.service_state = QStringLiteral("active");
            status_.main_pid = 49467;
            break;
        case Operation::STOP:
            status_.service_state = QStringLiteral("inactive");
            status_.main_pid = 0;
            break;
        case Operation::LOGS:
            detail
                = QStringLiteral("演示模式 · 板卡状态回读成功\n演示模式 · Live2D 服务状态已更新");
            break;
        case Operation::SAVE:
        case Operation::STATUS:
            break;
        }
        if (request.operation == Operation::SAVE || request.operation == Operation::APPLY) {
            status_.webrtc_enabled = request.payload.value(QStringLiteral("enabled")).toBool();
            status_.bind_address = request.payload.value(QStringLiteral("bindAddress")).toString();
        }
        status_.uvc_node_ready = status_.running();
        status_.uvc_host_state.clear();
        if (status_.uvc_node_ready) {
            status_.uvc_host_state = QStringLiteral("configured");
        }
        status_.rtsp_listening = status_.running();
        status_.signalling_listening = status_.running() && status_.webrtc_enabled;
        Q_EMIT operationFinished(request.operation, true, detail);
        Q_EMIT statusChanged(status_);
        busy_ = false;
        Q_EMIT busyChanged(false);
    });
}

} // namespace vtb::desktop
