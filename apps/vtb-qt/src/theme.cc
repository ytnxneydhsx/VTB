#include "theme.hh"

#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QString>

#include <initializer_list>
#include <utility>

namespace vtb::desktop {
namespace {

    class ColorToken {
    public:
        const char* key;
        const char* light;
        const char* dark;
    };

    constexpr ColorToken kColors[] {
        { "canvas", "#F6F8FD", "#161B24" },
        { "surface", "#FFFFFF", "#242C3A" },
        { "header", "#FEFBFF", "#1C222D" },
        { "sidebar", "#EDF1FB", "#202735" },
        { "text", "#182438", "#EBEFF8" },
        { "sub", "#46536B", "#C5CEE0" },
        { "muted", "#59677D", "#AAB7CE" },
        { "outline", "#D6DFF0", "#3C4A62" },
        { "edge", "#78869D", "#8294B1" },
        { "primary", "#345BA4", "#ADC6FF" },
        { "rateOut", "#007D76", "#67D9C7" },
        { "onPrimary", "#FFFFFF", "#102F65" },
        { "container", "#DDE7FF", "#2B4473" },
        { "onContainer", "#182E58", "#E2EBFF" },
        { "containerHint", "#415879", "#BDCCEA" },
        { "tonal", "#E1E9FA", "#3B4860" },
        { "onTonal", "#2E456C", "#DBE6FD" },
        { "hover", "#416AB4", "#BFD3FF" },
        { "pressed", "#284B8C", "#93B2F5" },
        { "focus", "#567EC5", "#BFD3FF" },
        { "good", "#DDF5E9", "#1F4639" },
        { "onGood", "#1A644C", "#A9E7CD" },
        { "warning", "#FFF0D2", "#4A3B20" },
        { "onWarning", "#785309", "#F5D08E" },
        { "error", "#FCE5E3", "#573033" },
        { "onError", "#973C43", "#FFB9BC" },
        { "disabled", "#E9EDF5", "#313845" },
        { "onDisabled", "#59667E", "#A7B3CA" },
        { "field", "#F7F9FE", "#202837" },
        { "switch", "#E2E8F4", "#303B50" },
    };

    auto value(const ColorToken& token, ColorMode mode) -> QString {
        if (mode == ColorMode::DARK) {
            return QString::fromLatin1(token.dark);
        }
        return QString::fromLatin1(token.light);
    }

    auto color(const char* key, ColorMode mode) -> QColor {
        for (const auto& token : kColors) {
            if (qstrcmp(token.key, key) == 0) {
                return QColor(value(token, mode));
            }
        }
        Q_UNREACHABLE();
    }

    constexpr auto kStyle = R"QSS(
QMainWindow, QWidget#content, QScrollArea, QWidget#page {
    background: @canvas@;
    color: @text@;
}
QWidget#header { background: @header@; border-bottom: 1px solid @outline@; }
QWidget#sidebar { background: @sidebar@; border-right: 1px solid @outline@; }
QFrame[panel="surface"] {
    background: @surface@;
    border: 1px solid @outline@;
    border-radius: 18px;
}
QFrame[panel="hero"] { background: @container@; border-radius: 20px; }
QLabel { color: @text@; background: transparent; }
QLabel[role="title"] { font-size: 30px; font-weight: 700; }
QLabel[role="heading"] { font-size: 18px; font-weight: 700; }
QLabel[role="brand"] { font-size: 15px; font-weight: 700; }
QLabel[role="sub"] { color: @sub@; font-size: 13px; }
QLabel[role="muted"] { color: @muted@; font-size: 12px; }
QLabel[role="monitorValue"] { font-size: 28px; font-weight: 700; }
QLabel[role="monitorSummaryValue"] { font-size: 20px; font-weight: 700; }
QLabel[role="monitorFrequency"] { font-size: 15px; font-weight: 600; }
QStackedWidget#videoPreviewSurface { background: #161B24; border-radius: 18px; }
QLabel#videoPreviewPlaceholder { background: transparent; color: #EBEFF8; font-size: 16px; }
QFrame#monitorDivider { background: @outline@; border: 0; }
QProgressBar#monitorMeter { background: @tonal@; border: 0; border-radius: 3px; }
QProgressBar#monitorMeter::chunk { background: @primary@; border-radius: 3px; }
QTabBar#monitorTabs::tab {
    padding: 7px 22px;
    margin-right: 6px;
    border: 1px solid transparent;
    border-radius: 15px;
    color: @sub@;
}
QTabBar#monitorTabs::tab:selected { background: @container@; color: @onContainer@; }
QTabBar#monitorTabs::tab:hover:!selected { background: @tonal@; }
QTableView#monitorProcessTable, QTableWidget#monitorDeviceTable {
    border: 0;
    background: @surface@;
    selection-background-color: @container@;
    selection-color: @onContainer@;
}
QTableView#monitorProcessTable::item, QTableWidget#monitorDeviceTable::item {
    border-bottom: 1px solid @outline@;
    padding: 0 6px;
}
QTableView#monitorProcessTable QHeaderView::section,
QTableWidget#monitorDeviceTable QHeaderView::section {
    background: @canvas@;
    color: @sub@;
    border: 0;
    padding: 6px 8px;
}
QLabel[role="monitorSeriesIn"] { color: @primary@; }
QLabel[role="monitorSeriesOut"] { color: @rateOut@; }
QLabel[role="hero-title"] { color: @onContainer@; font-size: 32px; font-weight: 700; }
QLabel[role="hero-label"] { color: @onContainer@; font-size: 14px; font-weight: 700; }
QLabel[role="hero-hint"] { color: @containerHint@; font-size: 13px; }
QLabel#statusBadge {
    border: 1px solid transparent;
    border-radius: 13px;
    padding: 3px 10px;
    min-height: 17px;
    font-size: 12px;
    font-weight: 600;
}
QLabel[tone="neutral"] { background: @tonal@; color: @onTonal@; }
QLabel[tone="good"] { background: @good@; color: @onGood@; }
QLabel[tone="warning"] { background: @warning@; color: @onWarning@; }
QLabel[tone="error"] { background: @error@; color: @onError@; }
QPushButton {
    padding: 0 18px;
    min-height: 38px;
    border-radius: 19px;
    border: 1px solid @edge@;
    background: @surface@;
    color: @text@;
    font-weight: 600;
}
QPushButton:hover { background: @tonal@; }
QPushButton:pressed { background: @container@; }
QPushButton:focus { border: 2px solid @focus@; }
QPushButton[kind="primary"] {
    border: 1px solid @primary@;
    background: @primary@;
    color: @onPrimary@;
}
QPushButton[kind="primary"]:hover { background: @hover@; border-color: @hover@; }
QPushButton[kind="primary"]:pressed { background: @pressed@; border-color: @pressed@; }
QPushButton[kind="primary"]:focus { border: 2px solid @focus@; }
QPushButton[kind="tonal"] {
    border: 1px solid transparent;
    background: @tonal@;
    color: @onTonal@;
}
QPushButton[kind="tonal"]:hover { border-color: @edge@; }
QPushButton[kind="tonal"]:pressed { background: @container@; }
QPushButton[kind="tonal"]:focus { border: 2px solid @focus@; }
QPushButton[kind="nav"] {
    background: transparent;
    text-align: left;
    border: 1px solid transparent;
    color: @sub@;
}
QPushButton[kind="nav"]:hover, QPushButton[kind="nav"]:checked {
    background: @container@;
    color: @onContainer@;
}
QPushButton[kind="nav"]:focus { border: 2px solid @focus@; }
QPushButton:disabled {
    background: @disabled@;
    border-color: transparent;
    color: @onDisabled@;
}
QLineEdit, QPlainTextEdit {
    border-radius: 12px;
    border: 1px solid @edge@;
    background: @field@;
    color: @text@;
    padding: 8px 12px;
}
QLineEdit { min-height: 22px; }
QPushButton#monitorJump { min-height: 28px; padding: 0 14px; border-radius: 14px; }
QLineEdit:focus, QPlainTextEdit:focus { border: 2px solid @focus@; }
QLineEdit[readOnly="true"], QLineEdit:disabled {
    background: @tonal@;
    color: @muted@;
}
QLineEdit[invalid="true"] { border: 2px solid @onError@; }
QDialog, QMessageBox { background: @surface@; color: @text@; }
)QSS";

} // namespace

auto applyTheme(QApplication& application, ColorMode mode) -> void {
    QPalette palette;
    for (const auto& [role, token] :
        std::initializer_list<std::pair<QPalette::ColorRole, const char*>> {
            { QPalette::Window, "canvas" }, { QPalette::Base, "surface" },
            { QPalette::WindowText, "text" }, { QPalette::Text, "text" },
            { QPalette::ButtonText, "text" }, { QPalette::Button, "switch" },
            { QPalette::Mid, "edge" }, { QPalette::PlaceholderText, "muted" },
            { QPalette::Highlight, "primary" }, { QPalette::HighlightedText, "onPrimary" },
            { QPalette::Link, "focus" }, { QPalette::LinkVisited, "rateOut" } }) {
        palette.setColor(role, color(token, mode));
    }
    palette.setColor(QPalette::Disabled, QPalette::Button, color("disabled", mode));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, color("onDisabled", mode));
    application.setPalette(palette);

    auto style = QString::fromLatin1(kStyle);
    for (const auto& token : kColors) {
        style.replace(QStringLiteral("@") + QString::fromLatin1(token.key) + QStringLiteral("@"),
            value(token, mode));
    }
    application.setStyleSheet(style);
}

} // namespace vtb::desktop
