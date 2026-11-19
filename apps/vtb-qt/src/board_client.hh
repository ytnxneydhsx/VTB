#pragma once

#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QTimer>

#include <optional>

namespace vtb::desktop {

enum class Operation { STATUS, START, STOP, SAVE, APPLY, LOGS };

class BoardStatus {
public:
    bool connected = false;
    QString load_state;
    QString service_state;
    QString service_result;
    QString revision;
    QString config_error;
    QString bind_address;
    QString rtsp_url;
    QString uvc_host_state;
    bool uvc_enabled = false;
    bool uvc_node_ready = false;
    bool rtsp_enabled = false;
    bool rtsp_listening = false;
    bool webrtc_enabled = false;
    bool signalling_listening = false;
    int main_pid = 0;
    int web_port = 8080;
    int signalling_port = 8443;

    auto running() const -> bool;
    auto changing() const -> bool;
    auto configAvailable() const -> bool;
    auto webRtcPreviewAvailable() const -> bool;
};

class BoardClient final : public QObject {
    Q_OBJECT

public:
    explicit BoardClient(bool demo, QObject* parent = nullptr);
    ~BoardClient() override;

    BoardClient(const BoardClient&) = delete;
    auto operator=(const BoardClient&) -> BoardClient& = delete;
    BoardClient(BoardClient&&) = delete;
    auto operator=(BoardClient&&) -> BoardClient& = delete;

    auto startMonitoring() -> void;
    auto refresh() -> void;
    auto operate(Operation operation, const QJsonObject& payload = { }) -> void;

Q_SIGNALS:
    void statusChanged(const BoardStatus& status);
    void busyChanged(bool busy);
    void operationFinished(Operation operation, bool success, const QString& detail);
    void connectionError(const QString& detail);

private:
    class Request {
    public:
        Operation operation;
        QJsonObject payload;
    };

    auto dispatch(const Request& request) -> void;
    auto finish(int exit_code, QProcess::ExitStatus status) -> void;
    auto complete(const QJsonObject& reply, const QString& transport_error = { }) -> void;
    auto simulate(const Request& request) -> void;
    auto publish(const QJsonObject& value) -> void;

    bool demo_;
    QByteArray script_;
    QProcess ssh_;
    QTimer interval_;
    QTimer deadline_;
    bool in_flight_ = false;
    bool busy_ = false;
    bool timed_out_ = false;
    Operation active_ = Operation::STATUS;
    std::optional<Request> waiting_;
    BoardStatus status_;
};

} // namespace vtb::desktop
