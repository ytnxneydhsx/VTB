#include "widgets.hh"

#include "theme.hh"

#include <QPainter>
#include <QStyle>

namespace vtb::desktop {

namespace {

    auto button_role(ButtonKind kind) -> QString {
        switch (kind) {
        case ButtonKind::PRIMARY:
            return QStringLiteral("primary");
        case ButtonKind::TONAL:
            return QStringLiteral("tonal");
        case ButtonKind::OUTLINE:
            return QStringLiteral("outline");
        case ButtonKind::NAVIGATION:
            return QStringLiteral("nav");
        }
        Q_UNREACHABLE();
    }

    auto tone_role(StatusTone tone) -> QString {
        switch (tone) {
        case StatusTone::NEUTRAL:
            return QStringLiteral("neutral");
        case StatusTone::GOOD:
            return QStringLiteral("good");
        case StatusTone::WARNING:
            return QStringLiteral("warning");
        case StatusTone::ERROR:
            return QStringLiteral("error");
        }
        Q_UNREACHABLE();
    }

} // namespace

auto repolish(QWidget& widget) -> void {
    widget.style()->unpolish(&widget);
    widget.style()->polish(&widget);
    widget.update();
}

ActionButton::ActionButton(const ButtonSpec& spec, QWidget* parent)
    : QPushButton(spec.text, parent) {
    setProperty("kind", button_role(spec.kind));
    setCursor(Qt::PointingHandCursor);
}

StatusBadge::StatusBadge(QWidget* parent)
    : QLabel(parent) {
    setObjectName(QStringLiteral("statusBadge"));
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
}

auto StatusBadge::setStatus(const QString& text, StatusTone tone) -> void {
    const auto role = tone_role(tone);
    if (property("tone") != role) {
        setProperty("tone", role);
        repolish(*this);
    }
    if (this->text() != text) {
        setText(text);
    }
}

StatusSwitch::StatusSwitch(QWidget* parent)
    : QAbstractButton(parent) {
    setCheckable(true);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    animation_.setDuration(metrics::kSwitchTransitionMs);
    animation_.setEasingCurve(QEasingCurve::OutCubic);
    connect(&animation_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        thumb_position_ = value.toReal();
        update();
    });
    connect(this, &QAbstractButton::toggled, this, [this](bool checked) {
        animation_.stop();
        animation_.setStartValue(thumb_position_);
        qreal target = 0.0;
        if (checked) {
            target = 1.0;
        }
        animation_.setEndValue(target);
        animation_.start();
    });
}

auto StatusSwitch::sizeHint() const -> QSize {
    return { metrics::kSwitchWidth, metrics::kSwitchHeight };
}

auto StatusSwitch::paintEvent(QPaintEvent*) -> void {
    const QRectF track = QRectF(rect()).adjusted(4, 6, -4, -6);
    const auto& colors = palette();
    QColor rail = colors.color(QPalette::Button);
    QColor knob = colors.color(QPalette::ButtonText);
    if (isChecked() && isEnabled()) {
        rail = colors.color(QPalette::Highlight);
        knob = colors.color(QPalette::HighlightedText);
    }
    QColor edge = colors.color(QPalette::Mid);
    if (isChecked() && isEnabled()) {
        edge = rail;
    }
    qreal knob_position = thumb_position_;
    if (isRightToLeft()) {
        knob_position = 1.0 - knob_position;
    }
    const auto radius = track.height() / 2.0;
    const auto center = QPointF(
        track.left() + radius + (track.width() - 2 * radius) * knob_position, track.center().y());
    const auto thumb_radius = 8.0 + 3.0 * thumb_position_;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    if (hasFocus()) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(colors.color(QPalette::Link), 2));
        painter.drawRoundedRect(track.adjusted(-3, -3, 3, 3), radius + 3, radius + 3);
    }
    painter.setBrush(rail);
    painter.setPen(QPen(edge, 1.5));
    painter.drawRoundedRect(track, radius, radius);
    if (isEnabled() && (underMouse() || isDown())) {
        QColor state_layer = knob;
        qreal opacity = 0.10;
        if (isDown()) {
            opacity = 0.18;
        }
        state_layer.setAlphaF(opacity);
        painter.setPen(Qt::NoPen);
        painter.setBrush(state_layer);
        painter.drawRoundedRect(track, radius, radius);
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(knob);
    painter.drawEllipse(center, thumb_radius, thumb_radius);
}

} // namespace vtb::desktop
