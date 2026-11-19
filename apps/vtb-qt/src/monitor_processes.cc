#include "monitor_processes.hh"

#include "board_client.hh"
#include "monitor_widgets.hh"
#include "theme.hh"
#include "widgets.hh"

#include <QAbstractTableModel>
#include <QApplication>
#include <QBrush>
#include <QFont>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonObject>
#include <QLineEdit>
#include <QScrollBar>
#include <QSortFilterProxyModel>
#include <QTableView>

#include <algorithm>

namespace vtb::desktop {
namespace {

constexpr int kSortRole = Qt::UserRole + 1;
constexpr int kSearchRole = Qt::UserRole + 2;

class ProcessRow {
public:
    int pid = 0;
    QString name;
    double cpu = -1;
    double rss = -1;
    QString user;
};

auto processCpu(double cpu) -> QString {
    if (cpu >= 0) {
        return QStringLiteral("%1%").arg(cpu, 0, 'f', 1);
    }
    return QStringLiteral("—");
}

} // namespace

class ProcessTableModel final : public QAbstractTableModel {
public:
    enum Column { PID, NAME, CPU, RSS, USER, COLUMN_COUNT };
    using QAbstractTableModel::QAbstractTableModel;

    auto rowCount(const QModelIndex& parent) const -> int override {
        if (parent.isValid()) {
            return 0;
        }
        return static_cast<int>(rows_.size());
    }

    auto columnCount(const QModelIndex& parent) const -> int override {
        if (parent.isValid()) {
            return 0;
        }
        return COLUMN_COUNT;
    }

    auto headerData(int column, Qt::Orientation orientation, int role) const -> QVariant override {
        if (orientation != Qt::Horizontal || column < 0 || column >= COLUMN_COUNT) {
            return {};
        }
        if (role == Qt::TextAlignmentRole) {
            if (column == CPU || column == RSS) {
                return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
            }
            return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
        }
        if (role != Qt::DisplayRole) {
            return {};
        }
        const QString titles[]{QStringLiteral("PID"), QStringLiteral("进程名"), QStringLiteral("CPU"),
            QStringLiteral("RSS"), QStringLiteral("用户")};
        return titles[column];
    }

    auto data(const QModelIndex& index, int role) const -> QVariant override {
        if (!index.isValid()) {
            return {};
        }
        const auto& row = rows_.at(index.row());
        const auto column = index.column();
        if (role == kSearchRole) {
            return QStringLiteral("%1 %2").arg(row.pid).arg(row.name);
        }
        if (role == Qt::BackgroundRole && row.pid == live_pid_) {
            auto color = QApplication::palette().color(QPalette::Highlight);
            color.setAlpha(28);
            return QBrush(color);
        }
        if (role == Qt::FontRole && row.pid == live_pid_) {
            auto font = QApplication::font();
            font.setBold(true);
            return font;
        }
        if (role == Qt::TextAlignmentRole && (column == RSS || column == CPU)) {
            return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
        }
        if (role == Qt::ToolTipRole && (column == NAME || column == USER)) {
            if (column == NAME) {
                return row.name;
            }
            return row.user;
        }
        if (role == Qt::DisplayRole || role == kSortRole) {
            switch (column) {
            case PID:
                return row.pid;
            case NAME:
                return row.name;
            case CPU:
                if (role == kSortRole) {
                    return row.cpu;
                }
                return processCpu(row.cpu);
            case RSS:
                if (role == kSortRole) {
                    return row.rss;
                }
                return monitorBytes(row.rss);
            case USER:
                if (role == Qt::DisplayRole && row.user.isEmpty()) {
                    return QStringLiteral("—");
                }
                return row.user;
            }
        }
        return {};
    }

    auto setRows(const QJsonArray& rows) -> void {
        QList<ProcessRow> next;
        next.reserve(rows.size());
        for (const auto& value : rows) {
            const auto row = value.toObject();
            const auto pid = row.value(QStringLiteral("pid")).toInt();
            if (pid <= 0) {
                continue;
            }
            next.append(ProcessRow{pid, row.value(QStringLiteral("name")).toString(),
                row.value(QStringLiteral("cpuPct")).toDouble(-1),
                row.value(QStringLiteral("rssBytes")).toDouble(-1),
                row.value(QStringLiteral("user")).toString()});
        }
        // Stable PID order gives tied values a consistent sort order across samples.
        std::sort(next.begin(), next.end(), [](const auto& a, const auto& b) { return a.pid < b.pid; });
        beginResetModel();
        rows_ = std::move(next);
        endResetModel();
    }

    auto setLivePid(int pid) -> void {
        if (live_pid_ == pid) {
            return;
        }
        live_pid_ = pid;
        if (!rows_.isEmpty()) {
            Q_EMIT dataChanged(index(0, 0), index(rows_.size() - 1, COLUMN_COUNT - 1),
                {Qt::FontRole, Qt::BackgroundRole});
        }
    }

    auto liveProcess() const -> const ProcessRow* {
        const auto match = std::find_if(rows_.cbegin(), rows_.cend(),
            [this](const auto& row) { return row.pid == live_pid_; });
        if (match != rows_.cend()) {
            return &*match;
        }
        return nullptr;
    }

private:
    QList<ProcessRow> rows_;
    int live_pid_ = 0;
};

MonitorProcesses::MonitorProcesses(QWidget* parent)
    : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(metrics::kSpacing);
    auto* live = monitorPanel(*layout);
    auto* metrics_row = new QHBoxLayout;
    metrics_row->setSpacing(20);
    live->addLayout(metrics_row);
    auto* service = new QVBoxLayout;
    auto* heading = new QHBoxLayout;
    heading->addWidget(monitorLabel(QStringLiteral("Live2D 主进程"), "heading"));
    badge_ = new StatusBadge;
    heading->addWidget(badge_);
    heading->addStretch();
    service->addLayout(heading);
    identity_ = monitorLabel({}, "muted");
    service->addWidget(identity_);
    service->addStretch();
    metrics_row->addLayout(service, 1);
    auto* cpu = new QVBoxLayout;
    cpu->addWidget(monitorLabel(QStringLiteral("CPU 占用"), "muted"));
    cpu_ = monitorLabel({}, "monitorValue");
    cpu->addWidget(cpu_);
    cpu->addWidget(monitorLabel(QStringLiteral("单核 100%，多核并行可超过 100%"), "muted"));
    metrics_row->addLayout(cpu, 1);
    auto* rss = new QVBoxLayout;
    rss->addWidget(monitorLabel(QStringLiteral("常驻内存（RSS）"), "muted"));
    rss_ = monitorLabel({}, "monitorValue");
    rss->addWidget(rss_);
    rss->addWidget(monitorLabel(QStringLiteral("当前常驻在物理内存的大小"), "muted"));
    metrics_row->addLayout(rss, 1);
    live->parentWidget()->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    auto* processes = monitorPanel(*layout);
    auto* controls = new QHBoxLayout;
    controls->addWidget(monitorLabel(QStringLiteral("全进程"), "heading"));
    count_ = monitorLabel({}, "muted");
    controls->addWidget(count_);
    controls->addStretch();
    auto* search = new QLineEdit;
    search->setPlaceholderText(QStringLiteral("搜索进程名或 PID"));
    search->setAccessibleName(search->placeholderText());
    search->setClearButtonEnabled(true);
    search->setMaximumWidth(288);
    controls->addWidget(search);
    processes->addLayout(controls);
    message_ = monitorLabel({}, "muted");
    processes->addWidget(message_);

    model_ = new ProcessTableModel(this);
    filter_ = new QSortFilterProxyModel(this);
    filter_->setSourceModel(model_);
    filter_->setSortRole(kSortRole);
    filter_->setSortCaseSensitivity(Qt::CaseInsensitive);
    filter_->setFilterRole(kSearchRole);
    filter_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    table_ = new QTableView;
    table_->setObjectName(QStringLiteral("monitorProcessTable"));
    table_->setAccessibleName(QStringLiteral("全进程"));
    table_->setModel(filter_);
    table_->setSortingEnabled(true);
    table_->sortByColumn(ProcessTableModel::CPU, Qt::DescendingOrder);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    table_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    table_->setShowGrid(false);
    table_->setWordWrap(false);
    table_->setMinimumHeight(250);
    table_->verticalHeader()->setDefaultSectionSize(30);
    table_->verticalHeader()->hide();
    table_->horizontalHeader()->setSectionResizeMode(ProcessTableModel::NAME, QHeaderView::Stretch);
    table_->setColumnWidth(ProcessTableModel::PID, 84);
    table_->setColumnWidth(ProcessTableModel::CPU, 96);
    table_->setColumnWidth(ProcessTableModel::RSS, 110);
    table_->setColumnWidth(ProcessTableModel::USER, 120);
    processes->addWidget(table_, 1);

    connect(search, &QLineEdit::textChanged, this, [this](const QString& value) {
        filter_->setFilterFixedString(value.trimmed());
        updateCount();
    });
    updateLive2d();
    updateCount();
}

auto MonitorProcesses::addOverview(QBoxLayout& parent, QWidget* action) -> void {
    auto* panel = monitorPanel(parent);
    auto* heading = new QHBoxLayout;
    heading->addWidget(monitorLabel(QStringLiteral("Live2D 进程"), "heading"));
    overview_badge_ = new StatusBadge;
    heading->addWidget(overview_badge_);
    heading->addStretch();
    heading->addWidget(action);
    panel->addLayout(heading);
    auto* values = new QHBoxLayout;
    overview_cpu_ = monitorLabel({}, "monitorSummaryValue");
    overview_rss_ = monitorLabel({}, "monitorSummaryValue");
    values->addWidget(monitorLabel(QStringLiteral("CPU"), "muted"));
    values->addWidget(overview_cpu_, 1);
    values->addWidget(monitorLabel(QStringLiteral("RSS"), "muted"));
    values->addWidget(overview_rss_, 1);
    panel->addLayout(values);
    updateLive2d();
}

auto MonitorProcesses::showProcesses(const QJsonArray& rows, bool readable) -> void {
    has_sample_ = true;
    readable_ = readable;
    const auto offset = table_->verticalScrollBar()->value();
    model_->setRows(rows);
    table_->verticalScrollBar()->setValue(offset);
    updateLive2d();
    updateCount();
}

auto MonitorProcesses::showStatus(const BoardStatus& status) -> void {
    service_known_ = status.connected;
    service_running_ = status.running();
    service_changing_ = status.changing();
    live_pid_ = 0;
    if (service_known_ && service_running_) {
        live_pid_ = status.main_pid;
    }
    model_->setLivePid(live_pid_);
    updateLive2d();
}

auto MonitorProcesses::showAvailability(bool supported, bool stale) -> void {
    if (supported_ == supported && stale_ == stale) {
        return;
    }
    supported_ = supported;
    stale_ = stale;
    if (!supported_) {
        has_sample_ = false;
        model_->setRows({});
    }
    updateLive2d();
    updateCount();
}

auto MonitorProcesses::updateLive2d() -> void {
    QString status;
    auto tone = StatusTone::NEUTRAL;
    const auto* row = model_->liveProcess();
    if (!supported_) {
        status = QStringLiteral("需更新板端采集器");
    } else if (stale_) {
        status = QStringLiteral("数据过期");
        tone = StatusTone::WARNING;
    } else if (service_known_ && service_changing_) {
        status = QStringLiteral("服务状态切换中");
    } else if (service_known_ && !service_running_) {
        status = QStringLiteral("未运行");
    } else if (!readable_) {
        status = QStringLiteral("进程数据不可读");
        tone = StatusTone::WARNING;
    } else if (row) {
        status = QStringLiteral("运行中");
        tone = StatusTone::GOOD;
    } else if (live_pid_ > 0 && has_sample_) {
        status = QStringLiteral("主进程暂不可见");
    } else {
        status = QStringLiteral("等待采样");
    }

    auto cpu = QStringLiteral("—");
    auto rss = QStringLiteral("—");
    auto identity = QStringLiteral("—");
    if (live_pid_ > 0) {
        identity = QStringLiteral("PID %1").arg(live_pid_);
    }
    if (row) {
        identity = QStringLiteral("%1 · PID %2").arg(row->name).arg(row->pid);
        cpu = processCpu(row->cpu);
        if (row->cpu < 0 && !stale_) {
            cpu = QStringLiteral("采样中");
        }
        rss = monitorBytes(row->rss);
    }
    identity_->setText(identity);
    cpu_->setText(cpu);
    rss_->setText(rss);
    badge_->setStatus(status, tone);
    if (overview_badge_) {
        overview_badge_->setStatus(status, tone);
        overview_cpu_->setText(cpu);
        overview_rss_->setText(rss);
    }
}

auto MonitorProcesses::updateCount() -> void {
    const auto total = model_->rowCount({});
    const auto matches = filter_->rowCount();
    auto count = QStringLiteral("%1 项").arg(total);
    if (matches != total) {
        count = QStringLiteral("%1 / %2 项").arg(matches).arg(total);
    }
    count_->setText(count);
    QString message;
    if (!supported_) {
        message = QStringLiteral("更新板端采集器后显示全进程。");
    } else if (!has_sample_) {
        message = QStringLiteral("等待进程采样");
    } else if (!readable_) {
        message = QStringLiteral("板端进程数据不可读");
    } else if (matches == 0) {
        message = QStringLiteral("没有匹配的进程");
    }
    message_->setText(message);
    message_->setVisible(!message.isEmpty());
}

} // namespace vtb::desktop
