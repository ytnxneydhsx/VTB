#pragma once

#include <QWidget>

class QBoxLayout;
class QJsonArray;
class QLabel;
class QSortFilterProxyModel;
class QTableView;

namespace vtb::desktop {

class BoardStatus;
class ProcessTableModel;
class StatusBadge;

class MonitorProcesses final : public QWidget {
public:
    explicit MonitorProcesses(QWidget* parent = nullptr);

    auto addOverview(QBoxLayout& parent, QWidget* action) -> void;
    auto showProcesses(const QJsonArray& rows, bool readable) -> void;
    auto showStatus(const BoardStatus& status) -> void;
    auto showAvailability(bool supported, bool stale) -> void;

private:
    auto updateLive2d() -> void;
    auto updateCount() -> void;

    ProcessTableModel* model_ = nullptr;
    QSortFilterProxyModel* filter_ = nullptr;
    QTableView* table_ = nullptr;
    QLabel* count_ = nullptr;
    QLabel* message_ = nullptr;
    QLabel* identity_ = nullptr;
    QLabel* cpu_ = nullptr;
    QLabel* rss_ = nullptr;
    StatusBadge* badge_ = nullptr;
    QLabel* overview_cpu_ = nullptr;
    QLabel* overview_rss_ = nullptr;
    StatusBadge* overview_badge_ = nullptr;
    int live_pid_ = 0;
    bool service_known_ = false;
    bool service_running_ = false;
    bool service_changing_ = false;
    bool supported_ = true;
    bool stale_ = false;
    bool has_sample_ = false;
    bool readable_ = true;
};

} // namespace vtb::desktop
