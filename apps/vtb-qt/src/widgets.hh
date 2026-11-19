#pragma once

#include <QAbstractButton>
#include <QLabel>
#include <QPushButton>
#include <QVariantAnimation>

namespace vtb::desktop {

enum class ButtonKind { PRIMARY, TONAL, OUTLINE, NAVIGATION };
enum class StatusTone { NEUTRAL, GOOD, WARNING, ERROR };

class ButtonSpec {
public:
    QString text;
    ButtonKind kind = ButtonKind::OUTLINE;
};

class ActionButton final : public QPushButton {
public:
    explicit ActionButton(const ButtonSpec& spec, QWidget* parent = nullptr);
};

class StatusBadge final : public QLabel {
public:
    explicit StatusBadge(QWidget* parent = nullptr);
    auto setStatus(const QString& text, StatusTone tone) -> void;
};

class StatusSwitch final : public QAbstractButton {
public:
    explicit StatusSwitch(QWidget* parent = nullptr);
    ~StatusSwitch() override = default;

    StatusSwitch(const StatusSwitch&) = delete;
    auto operator=(const StatusSwitch&) -> StatusSwitch& = delete;
    StatusSwitch(StatusSwitch&&) = delete;
    auto operator=(StatusSwitch&&) -> StatusSwitch& = delete;

    auto sizeHint() const -> QSize override;

protected:
    auto paintEvent(QPaintEvent* event) -> void override;

private:
    QVariantAnimation animation_;
    qreal thumb_position_ = 0.0;
};

auto repolish(QWidget& widget) -> void;

} // namespace vtb::desktop
