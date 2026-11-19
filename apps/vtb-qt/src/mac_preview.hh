#pragma once

#include <QString>
#include <QVector>

#include <functional>
#include <memory>

class QWidget;

namespace vtb::desktop {

class CameraChoice {
public:
    QString id;
    QString name;
};

enum class CameraPermission { UNDETERMINED, ALLOWED, DENIED };

class MacPreview final {
public:
    explicit MacPreview(QWidget& host);
    ~MacPreview();
    MacPreview(const MacPreview&) = delete;
    auto operator=(const MacPreview&) -> MacPreview& = delete;
    MacPreview(MacPreview&&) = delete;
    auto operator=(MacPreview&&) -> MacPreview& = delete;

    static auto cameras() -> QVector<CameraChoice>;
    static auto cameraPermission() -> CameraPermission;
    static auto requestCameraPermission(std::function<void(bool)> reply) -> void;
    auto startCamera(const QString& id) -> void;
    auto stop() -> void;
    auto frames() const -> quint64;
    auto error() const -> QString;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vtb::desktop
