#pragma once

#include <QFrame>
#include <QLabel>
#include <QVBoxLayout>

namespace vtb::desktop {

inline auto monitorLabel(const QString& text = {}, const char* role = "") -> QLabel* {
    auto* label = new QLabel(text);
    label->setProperty("role", role);
    label->setTextFormat(Qt::PlainText);
    return label;
}

inline auto monitorPanel(QBoxLayout& parent, const QString& title = {}) -> QVBoxLayout* {
    auto* frame = new QFrame;
    frame->setProperty("panel", "surface");
    auto* content = new QVBoxLayout(frame);
    content->setContentsMargins(16, 12, 16, 14);
    content->setSpacing(6);
    if (!title.isEmpty()) {
        content->addWidget(monitorLabel(title, "heading"));
    }
    parent.addWidget(frame, 1);
    return content;
}

inline auto monitorBytes(double bytes, bool per_second = false) -> QString {
    if (bytes < 0) {
        return QStringLiteral("—");
    }
    constexpr double kib = 1024;
    constexpr double mib = kib * 1024;
    constexpr double gib = mib * 1024;
    QString text;
    if (bytes >= gib) {
        text = QStringLiteral("%1 GiB").arg(bytes / gib, 0, 'f', 1);
    } else if (bytes >= mib) {
        text = QStringLiteral("%1 MiB").arg(bytes / mib, 0, 'f', 1);
    } else if (bytes >= kib) {
        text = QStringLiteral("%1 KiB").arg(bytes / kib, 0, 'f', 1);
    } else if (bytes > 0 && bytes < 1) {
        text = QStringLiteral("<1 B");
    } else {
        text = QStringLiteral("%1 B").arg(bytes, 0, 'f', 0);
    }
    if (per_second) {
        text += QStringLiteral("/s");
    }
    return text;
}

} // namespace vtb::desktop
