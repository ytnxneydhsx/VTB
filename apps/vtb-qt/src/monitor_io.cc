#include "monitor_io.hh"

#include "monitor_widgets.hh"
#include "theme.hh"

#include <QHeaderView>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QPainter>
#include <QPainterPath>
#include <QSignalBlocker>
#include <QTableWidget>

#include <algorithm>

namespace vtb::desktop {
namespace {

constexpr qint64 kWindowMs = 60000;
constexpr qint64 kGapMs = 4500;

class RateSample {
public:
    qint64 ms = 0;
    double in = -1;
    double out = -1;
};

class Device {
public:
    QString name;
    QString state;
    QList<RateSample> history;
};

auto linkState(const QString& name, const QString& state) -> QString {
    if (state == QStringLiteral("up")) {
        if (name == QStringLiteral("end1")) {
            return QStringLiteral("有线直连");
        }
        return QStringLiteral("已连接");
    }
    if (state == QStringLiteral("down") || state == QStringLiteral("lowerlayerdown")) {
        return QStringLiteral("未连接");
    }
    if (state == QStringLiteral("dormant")) {
        return QStringLiteral("休眠");
    }
    return QStringLiteral("—");
}

auto seriesColor(const QPalette& palette, bool outgoing) -> QColor {
    if (!outgoing) {
        return palette.color(QPalette::Highlight);
    }
    return palette.color(QPalette::LinkVisited);
}

class RateChart final : public QWidget {
public:
    explicit RateChart(QWidget* parent = nullptr)
        : QWidget(parent) {
        setMinimumHeight(135);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    auto showHistory(const QList<RateSample>& samples, qint64 now, const QString& message) -> void {
        samples_ = samples;
        now_ = now;
        message_ = message;
        setAccessibleDescription(message);
        update();
    }

protected:
    auto paintEvent(QPaintEvent*) -> void override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(palette().color(QPalette::PlaceholderText));
        if (!message_.isEmpty()) {
            painter.drawText(rect(), Qt::AlignCenter, message_);
            return;
        }

        double peak = 1;
        for (const auto& sample : samples_) {
            peak = std::max({peak, sample.in, sample.out});
        }
        const auto peak_label = monitorBytes(peak, true);
        const auto left = fontMetrics().horizontalAdvance(peak_label) + 9;
        const auto vertical_margin = fontMetrics().height() + 5;
        const QRectF plot(left, vertical_margin, width() - left - 6, height() - 2 * vertical_margin);
        if (plot.width() <= 0 || plot.height() <= 0) {
            return;
        }
        auto grid = palette().color(QPalette::Mid);
        grid.setAlpha(95);
        painter.setPen(grid);
        for (const double y : {plot.top(), plot.center().y(), plot.bottom()}) {
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        }
        painter.setPen(palette().color(QPalette::PlaceholderText));
        painter.drawText(QPointF(0, plot.top() + 4), peak_label);
        painter.drawText(QPointF(0, plot.bottom() + 4), QStringLiteral("0 B/s"));
        const auto baseline = plot.bottom() + fontMetrics().height();
        const auto center = QStringLiteral("−30 s");
        const auto present = QStringLiteral("现在");
        painter.drawText(QPointF(plot.left(), baseline), QStringLiteral("−60 s"));
        painter.drawText(QPointF(plot.center().x() - fontMetrics().horizontalAdvance(center) / 2.0,
            baseline), center);
        painter.drawText(QPointF(plot.right() - fontMetrics().horizontalAdvance(present), baseline),
            present);

        for (const bool outgoing : {false, true}) {
            QPainterPath path;
            QPointF newest;
            qint64 previous = 0;
            for (const auto& sample : samples_) {
                auto value = sample.in;
                if (outgoing) {
                    value = sample.out;
                }
                if (value < 0) {
                    previous = 0;
                    continue;
                }
                const double x = 1.0 - static_cast<double>(now_ - sample.ms) / kWindowMs;
                newest = { plot.left() + plot.width() * x,
                    plot.bottom() - plot.height() * value / peak };
                if (previous == 0 || sample.ms - previous > kGapMs) {
                    path.moveTo(newest);
                } else {
                    path.lineTo(newest);
                }
                previous = sample.ms;
            }
            const auto color = seriesColor(palette(), outgoing);
            painter.setPen(QPen(color, 1.7));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(path);
            if (previous > 0) {
                painter.setBrush(color);
                painter.drawEllipse(newest, 2.1, 2.1);
            }
        }
    }

private:
    QList<RateSample> samples_;
    qint64 now_ = 0;
    QString message_;
};

} // namespace

class MonitorIoGroup final : public QObject {
public:
    MonitorIoGroup(QBoxLayout& charts, QBoxLayout& tables, bool network, QObject* parent)
        : QObject(parent)
        , network_(network) {
        auto* graph_panel = monitorPanel(charts);
        auto* heading = new QHBoxLayout;
        QString title = QStringLiteral("磁盘速率");
        QString table_title = QStringLiteral("块设备");
        QString first_col = QStringLiteral("设备");
        QString second_col = QStringLiteral("类型");
        QString in = QStringLiteral("读取");
        QString out = QStringLiteral("写入");
        if (network_) {
            title = QStringLiteral("网络速率");
            table_title = QStringLiteral("网络接口");
            first_col = QStringLiteral("接口");
            second_col = QStringLiteral("链路状态");
            in = QStringLiteral("接收");
            out = QStringLiteral("发送");
        }
        heading->addWidget(monitorLabel(title, "heading"));
        heading->addStretch();
        name_ = monitorLabel({}, "sub");
        heading->addWidget(name_);
        graph_panel->addLayout(heading);
        auto* metrics = new QGridLayout;
        auto* in_legend = monitorLabel(QStringLiteral("● %1").arg(in), "monitorSeriesIn");
        auto* out_legend = monitorLabel(QStringLiteral("● %1").arg(out), "monitorSeriesOut");
        metrics->addWidget(in_legend, 0, 0);
        metrics->addWidget(out_legend, 0, 1);
        in_ = monitorLabel({}, "monitorValue");
        out_ = monitorLabel({}, "monitorValue");
        metrics->addWidget(in_, 1, 0);
        metrics->addWidget(out_, 1, 1);
        metrics->setColumnStretch(0, 1);
        metrics->setColumnStretch(1, 1);
        graph_panel->addLayout(metrics);
        chart_ = new RateChart;
        chart_->setAccessibleName(title);
        graph_panel->addWidget(chart_, 1);
        graph_panel->addWidget(monitorLabel(QStringLiteral("最近 60 秒"), "muted"));

        auto* table_panel = monitorPanel(tables, table_title);
        table_ = new QTableWidget(0, 4);
        table_->setObjectName(QStringLiteral("monitorDeviceTable"));
        table_->setAccessibleName(table_title);
        table_->setHorizontalHeaderLabels({first_col, second_col, in, out});
        table_->verticalHeader()->hide();
        table_->verticalHeader()->setDefaultSectionSize(30);
        table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        table_->setShowGrid(false);
        table_->setWordWrap(false);
        table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        // QTBUG-149612: Cocoa can invalidate the table interface while reading selected cells.
        // Use the current row for chart navigation and leave Qt's item selection empty.
        table_->setSelectionMode(QAbstractItemView::NoSelection);
        table_->setMinimumHeight(134);
        table_panel->addWidget(table_, 1);
        if (!network_) {
            table_panel->addWidget(monitorLabel(QStringLiteral("同一块盘按整盘统计"), "muted"));
        }

        connect(table_, &QTableWidget::currentCellChanged, this, [this](int row, int, int, int) {
            if (const auto* item = table_->item(row, 0)) {
                selected_ = item->data(Qt::UserRole).toString();
                updateRowEmphasis();
                updateChart();
            }
        });
        updateChart();
    }

    auto showSample(const QJsonValue& rows, qint64 now) -> void {
        now_ = now;
        has_sample_ = true;
        readable_ = rows.isArray();
        QMap<QString, Device> updated;
        QString preferred;
        for (const auto& value : rows.toArray()) {
            const auto row = value.toObject();
            const auto id = row.value(QStringLiteral("id")).toString();
            const auto name = row.value(QStringLiteral("name")).toString();
            if (id.isEmpty() || name.isEmpty()) {
                continue;
            }
            auto device = devices_.take(id);
            device.name = name;
            device.state = row.value(QStringLiteral("state")).toString();
            while (!device.history.isEmpty() && now - device.history.first().ms > kWindowMs) {
                device.history.removeFirst();
            }
            device.history.append(RateSample{now, row.value(QStringLiteral("inBps")).toDouble(-1),
                row.value(QStringLiteral("outBps")).toDouble(-1)});
            updated.insert(id, std::move(device));
            if ((network_ && name == QStringLiteral("end1"))
                || (!network_ && name == QStringLiteral("mmcblk0"))) {
                preferred = id;
            }
        }
        devices_ = std::move(updated);
        if (!devices_.contains(selected_)) {
            selected_ = preferred;
            if (selected_.isEmpty() && !devices_.isEmpty()) {
                selected_ = devices_.firstKey();
            }
        }
        updateTable();
        updateChart();
    }

    auto clear() -> void {
        has_sample_ = false;
        devices_.clear();
        table_->setRowCount(0);
    }

    auto showAvailability(bool supported, bool stale) -> void {
        supported_ = supported;
        stale_ = stale;
        if (!supported_) {
            clear();
        }
        updateChart();
    }

    auto summary() const -> QString {
        const auto preferred = devices_.constFind(selected_);
        if (preferred == devices_.cend()) {
            return QStringLiteral("—");
        }
        const auto sample = preferred->history.last();
        if (network_) {
            return QStringLiteral("%1  ↓ %2  ↑ %3")
                .arg(preferred->name, monitorBytes(sample.in, true), monitorBytes(sample.out, true));
        }
        return QStringLiteral("%1  读 %2  写 %3")
            .arg(preferred->name, monitorBytes(sample.in, true), monitorBytes(sample.out, true));
    }

    auto table() const -> QTableWidget* { return table_; }

private:
    auto updateTable() -> void {
        const QSignalBlocker block(table_);
        table_->setRowCount(devices_.size());
        int row = 0;
        for (auto device = devices_.cbegin(); device != devices_.cend(); ++device, ++row) {
            auto state = device->state;
            if (network_) {
                state = linkState(device->name, state);
            } else if (state.isEmpty() || state == QStringLiteral("block")) {
                state = QStringLiteral("物理磁盘");
            }
            const auto sample = device->history.last();
            const QString items[]{device->name, state, monitorBytes(sample.in, true),
                monitorBytes(sample.out, true)};
            for (int column = 0; column < 4; ++column) {
                auto* cell = table_->item(row, column);
                if (!cell) {
                    cell = new QTableWidgetItem;
                    table_->setItem(row, column, cell);
                }
                cell->setText(items[column]);
                if (column == 0) {
                    cell->setData(Qt::UserRole, device.key());
                } else if (column >= 2) {
                    cell->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
                }
            }
            if (device.key() == selected_ && table_->currentRow() != row) {
                table_->setCurrentCell(row, 0, QItemSelectionModel::NoUpdate);
            }
        }
        updateRowEmphasis();
    }

    auto updateRowEmphasis() -> void {
        auto accent = table_->palette().color(QPalette::Highlight);
        accent.setAlpha(32);
        for (int row = 0; row < table_->rowCount(); ++row) {
            const auto* name = table_->item(row, 0);
            if (!name) {
                continue;
            }
            const bool active = name->data(Qt::UserRole).toString() == selected_;
            QBrush background;
            if (active) {
                background = QBrush(accent);
            }
            for (int column = 0; column < table_->columnCount(); ++column) {
                auto* cell = table_->item(row, column);
                auto font = cell->font();
                font.setBold(active);
                cell->setFont(font);
                cell->setBackground(background);
            }
        }
    }

    auto updateChart() -> void {
        const auto device = devices_.constFind(selected_);
        if (device == devices_.cend()) {
            name_->clear();
            in_->setText(QStringLiteral("—"));
            out_->setText(QStringLiteral("—"));
            QString message = QStringLiteral("等待采样");
            if (!supported_) {
                message = QStringLiteral("需更新板端采集器");
            } else if (stale_) {
                message = QStringLiteral("数据过期");
            } else if (has_sample_ && !readable_) {
                message = QStringLiteral("板端计数器不可读");
            } else if (has_sample_) {
                message = QStringLiteral("未发现设备");
            }
            chart_->showHistory({}, now_, message);
            return;
        }
        const auto sample = device->history.last();
        name_->setText(device->name);
        in_->setText(monitorBytes(sample.in, true));
        out_->setText(monitorBytes(sample.out, true));
        QString message;
        if (std::none_of(device->history.cbegin(), device->history.cend(),
                [](const auto& point) { return point.in >= 0 || point.out >= 0; })) {
            message = QStringLiteral("采样中");
        }
        chart_->showHistory(device->history, now_, message);
    }

    bool network_;
    bool supported_ = true;
    bool stale_ = false;
    bool has_sample_ = false;
    bool readable_ = true;
    qint64 now_ = 0;
    QMap<QString, Device> devices_;
    QString selected_;
    QLabel* name_ = nullptr;
    QLabel* in_ = nullptr;
    QLabel* out_ = nullptr;
    RateChart* chart_ = nullptr;
    QTableWidget* table_ = nullptr;
};

MonitorIo::MonitorIo(QWidget* parent)
    : QWidget(parent) {
    auto* content = new QVBoxLayout(this);
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(metrics::kSpacing);
    auto* charts = new QHBoxLayout;
    auto* tables = new QHBoxLayout;
    charts->setSpacing(metrics::kSpacing);
    tables->setSpacing(metrics::kSpacing);
    content->addLayout(charts, 3);
    content->addLayout(tables, 2);
    network_ = new MonitorIoGroup(*charts, *tables, true, this);
    disks_ = new MonitorIoGroup(*charts, *tables, false, this);
    connect(network_->table(), &QTableWidget::currentCellChanged, this, [this] { updateOverview(); });
    connect(disks_->table(), &QTableWidget::currentCellChanged, this, [this] { updateOverview(); });
}

auto MonitorIo::addOverview(QBoxLayout& parent, QWidget* action) -> void {
    auto* panel = monitorPanel(parent);
    auto* heading = new QHBoxLayout;
    heading->addWidget(monitorLabel(QStringLiteral("网络与磁盘"), "heading"));
    heading->addStretch();
    heading->addWidget(action);
    panel->addLayout(heading);
    overview_network_ = monitorLabel({}, "sub");
    overview_disks_ = monitorLabel({}, "sub");
    panel->addWidget(overview_network_);
    panel->addWidget(overview_disks_);
    updateOverview();
}

auto MonitorIo::showSample(const QJsonObject& io) -> void {
    const auto now = io.value(QStringLiteral("timeMs")).toInteger();
    if (now <= 0) {
        return;
    }
    if (now <= time_ms_) {
        network_->clear();
        disks_->clear();
    }
    time_ms_ = now;
    network_->showSample(io.value(QStringLiteral("network")), now);
    disks_->showSample(io.value(QStringLiteral("disks")), now);
    updateOverview();
}

auto MonitorIo::showAvailability(bool supported, bool stale) -> void {
    if (supported_ == supported && stale_ == stale) {
        return;
    }
    supported_ = supported;
    stale_ = stale;
    network_->showAvailability(supported, stale);
    disks_->showAvailability(supported, stale);
    updateOverview();
}

auto MonitorIo::updateOverview() -> void {
    if (!overview_network_) {
        return;
    }
    if (!supported_) {
        overview_network_->setText(QStringLiteral("需更新板端采集器"));
        overview_disks_->clear();
        return;
    }
    overview_network_->setText(network_->summary());
    overview_disks_->setText(disks_->summary());
}

} // namespace vtb::desktop
