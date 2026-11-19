#include "main_window.hh"

#include "board_client.hh"
#include "board_monitor.hh"
#include "board_monitor_page.hh"
#include "theme.hh"
#include "video_preview.hh"
#include "widgets.hh"

#include <QApplication>
#include <QCloseEvent>
#include <QDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHostAddress>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QStackedWidget>

namespace vtb::desktop {
namespace {

    auto text(const QString& value, const char* role = "") -> QLabel* {
        auto* label = new QLabel(value);
        label->setProperty("role", role);
        label->setTextFormat(Qt::PlainText);
        return label;
    }

    auto card(const char* kind = "surface") -> QFrame* {
        auto* frame = new QFrame;
        frame->setProperty("panel", kind);
        return frame;
    }

    auto column(QWidget* parent, int gap = metrics::kSpacing, int margin = 0) -> QVBoxLayout* {
        auto* layout = new QVBoxLayout(parent);
        layout->setSpacing(gap);
        layout->setContentsMargins(margin, margin, margin, margin);
        return layout;
    }

    auto row(QWidget* parent, int gap = metrics::kSpacing, int margin = 0) -> QHBoxLayout* {
        auto* layout = new QHBoxLayout(parent);
        layout->setSpacing(gap);
        layout->setContentsMargins(margin, margin, margin, margin);
        return layout;
    }

    auto subtitle(QVBoxLayout& layout, const QString& title, const QString& hint) -> void {
        auto* heading = new QWidget;
        auto* lines = column(heading, 6);
        lines->addWidget(text(title, "title"));
        lines->addWidget(text(hint, "sub"));
        layout.addWidget(heading);
    }

    auto page(QWidget*& content) -> QScrollArea* {
        auto* scroll = new QScrollArea;
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        content = new QWidget;
        content->setObjectName(QStringLiteral("page"));
        scroll->setWidget(content);
        return scroll;
    }

    auto description(QVBoxLayout& layout, const QString& value) -> QLabel* {
        auto* label = text(value, "sub");
        label->setWordWrap(true);
        layout.addWidget(label);
        return label;
    }

    auto service_label(const BoardStatus& status) -> QString {
        if (status.load_state.isEmpty()) {
            return QStringLiteral("待回读");
        }
        if (status.load_state != QStringLiteral("loaded")) {
            return QStringLiteral("服务未安装");
        }
        if (status.running()) {
            return QStringLiteral("运行中");
        }
        if (status.service_state == QStringLiteral("activating")) {
            return QStringLiteral("启动中");
        }
        if (status.service_state == QStringLiteral("deactivating")) {
            return QStringLiteral("停止中");
        }
        if (status.service_state == QStringLiteral("reloading")) {
            return QStringLiteral("重载中");
        }
        if (status.service_state == QStringLiteral("failed")) {
            return QStringLiteral("服务失败");
        }
        return QStringLiteral("已停止");
    }

    auto operation_label(Operation op) -> QString {
        switch (op) {
        case Operation::START:
            return QStringLiteral("板端已返回启动命令结果。");
        case Operation::STOP:
            return QStringLiteral("板端已返回停止命令结果。");
        case Operation::SAVE:
            return QStringLiteral("配置已保存，下次启动 Live2D 时生效。");
        case Operation::APPLY:
            return QStringLiteral("配置已写入，正在回读重启后的状态。");
        case Operation::LOGS:
        case Operation::STATUS:
            return { };
        }
        Q_UNREACHABLE();
    }

} // namespace

class MainWindow::Impl final {
public:
    Impl(MainWindow& window, bool demo)
        : window_(window)
        , demo_(demo)
        , board_(demo)
        , monitor_(demo) {
        build_window();
        bind_events();
        update_view();
        board_.startMonitoring();
        monitor_.start();
    }

    auto busy() const -> bool { return busy_; }

private:
    enum class Page { DASHBOARD, SETTINGS, MONITOR, UVC, RTSP, WEBRTC };

    auto build_window() -> void {
        window_.setWindowTitle(QStringLiteral("VTB 上位机"));
        window_.setMinimumSize(850, 640);
        window_.resize(1200, 780);
        auto* root = new QWidget;
        window_.setCentralWidget(root);
        auto* layout = column(root, 0);

        auto* header = new QWidget;
        header->setObjectName(QStringLiteral("header"));
        auto* top = row(header, 10);
        top->setContentsMargins(22, 8, 22, 8);
        top->addWidget(text(QStringLiteral("VTB 上位机"), "brand"));
        if (demo_) {
            auto* marker = new StatusBadge;
            marker->setStatus(QStringLiteral("本地演示"), StatusTone::NEUTRAL);
            top->addWidget(marker);
        }
        top->addStretch();
        link_ = new StatusBadge;
        top->addWidget(link_);
        theme_ = new ActionButton({ QStringLiteral("深色"), ButtonKind::TONAL });
        theme_->setCheckable(true);
        top->addWidget(theme_);
        layout->addWidget(header);

        auto* body = new QWidget;
        auto* horizontal = row(body, 0);
        auto* sidebar = new QWidget;
        sidebar->setObjectName(QStringLiteral("sidebar"));
        sidebar->setFixedWidth(metrics::kSidebarWidth);
        auto* navigation = column(sidebar, 12, 17);
        navigation->addWidget(text(QStringLiteral("RK3588"), "heading"));
        navigation->addWidget(text(QStringLiteral("192.168.77.2"), "muted"));
        navigation->addSpacing(10);
        dashboard_nav_ = new ActionButton({ QStringLiteral("控制台"), ButtonKind::NAVIGATION });
        monitor_nav_ = new ActionButton({ QStringLiteral("板卡监控"), ButtonKind::NAVIGATION });
        monitor_nav_->setObjectName(QStringLiteral("boardMonitorNav"));
        settings_nav_ = new ActionButton({ QStringLiteral("WebRTC 设置"), ButtonKind::NAVIGATION });
        dashboard_nav_->setCheckable(true);
        monitor_nav_->setCheckable(true);
        settings_nav_->setCheckable(true);
        navigation->addWidget(dashboard_nav_);
        navigation->addWidget(monitor_nav_);
        navigation->addWidget(text(QStringLiteral("视频预览"), "muted"));
        uvc_nav_ = new ActionButton({ QStringLiteral("UVC"), ButtonKind::NAVIGATION });
        rtsp_nav_ = new ActionButton({ QStringLiteral("RTSP"), ButtonKind::NAVIGATION });
        webrtc_nav_ = new ActionButton({ QStringLiteral("WebRTC"), ButtonKind::NAVIGATION });
        for (auto* button : { uvc_nav_, rtsp_nav_, webrtc_nav_ }) {
            button->setCheckable(true);
            navigation->addWidget(button);
        }
        navigation->addWidget(settings_nav_);
        navigation->addStretch();
        refresh_ = new ActionButton({ QStringLiteral("刷新状态"), ButtonKind::TONAL });
        navigation->addWidget(refresh_);
        navigation->addWidget(text(QStringLiteral("有线直连 · rk3588-direct"), "muted"));
        horizontal->addWidget(sidebar);

        stack_ = new QStackedWidget;
        stack_->setObjectName(QStringLiteral("content"));
        stack_->addWidget(build_dashboard());
        stack_->addWidget(build_settings());
        monitor_page_ = new BoardMonitorPage;
        stack_->addWidget(monitor_page_);
        video_page_ = new VideoPreview(demo_);
        stack_->addWidget(video_page_);
        horizontal->addWidget(stack_, 1);
        layout->addWidget(body, 1);
        show_page(Page::DASHBOARD);
    }

    auto build_dashboard() -> QWidget* {
        QWidget* body = nullptr;
        auto* scroll = page(body);
        auto* layout = column(body, 16, metrics::kContentMargin);
        subtitle(*layout, QStringLiteral("设备控制台"),
            QStringLiteral("管理 Live2D 服务和查看运行日志。"));

        auto* hero = card("hero");
        auto* hero_row = row(hero, 22, 22);
        auto* copy = new QVBoxLayout;
        copy->setSpacing(8);
        copy->addWidget(text(QStringLiteral("Live2D"), "hero-label"));
        service_ = text({ }, "hero-title");
        copy->addWidget(service_);
        hero_hint_ = text({ }, "hero-hint");
        hero_hint_->setWordWrap(true);
        copy->addWidget(hero_hint_);
        hero_row->addLayout(copy, 1);
        service_button_ = new ActionButton({ QStringLiteral("启动 Live2D"), ButtonKind::PRIMARY });
        service_button_->setMinimumWidth(142);
        hero_row->addWidget(service_button_, 0, Qt::AlignVCenter);
        layout->addWidget(hero);

        auto* recent = card();
        auto* recent_row = row(recent, 14, 16);
        auto* details = new QVBoxLayout;
        details->setSpacing(8);
        details->addWidget(text(QStringLiteral("最近事件"), "heading"));
        event_ = description(*details, QStringLiteral("正在通过有线 SSH 读取板端状态。"));
        recent_row->addLayout(details, 1);
        logs_ = new ActionButton({ QStringLiteral("查看运行日志"), ButtonKind::OUTLINE });
        recent_row->addWidget(logs_, 0, Qt::AlignVCenter);
        layout->addWidget(recent);
        layout->addStretch();
        return scroll;
    }

    auto build_settings() -> QWidget* {
        QWidget* body = nullptr;
        auto* scroll = page(body);
        auto* layout = column(body, 16, metrics::kContentMargin);
        subtitle(*layout, QStringLiteral("WebRTC 设置"),
            QStringLiteral("管理浏览器视频输出与板端监听地址。"));

        auto* output = card();
        auto* section = column(output, 12, 18);
        section->addWidget(text(QStringLiteral("WebRTC 输出"), "heading"));
        auto* toggle_row = new QHBoxLayout;
        auto* note = new QVBoxLayout;
        auto* switch_label = text(QStringLiteral("启用 WebRTC"));
        note->addWidget(switch_label);
        description(*note, QStringLiteral("允许浏览器接收 Live2D 视频。"));
        toggle_row->addLayout(note, 1);
        enabled_ = new StatusSwitch;
        enabled_->setAccessibleName(QStringLiteral("启用 WebRTC"));
        switch_label->setBuddy(enabled_);
        toggle_row->addWidget(enabled_);
        section->addLayout(toggle_row);
        auto* applied_row = new QHBoxLayout;
        applied_ = text({ }, "muted");
        applied_row->addWidget(applied_);
        applied_row->addStretch();
        draft_mark_ = new StatusBadge;
        draft_mark_->setStatus(QStringLiteral("未保存"), StatusTone::WARNING);
        applied_row->addWidget(draft_mark_);
        section->addLayout(applied_row);
        layout->addWidget(output);

        auto* network = card();
        auto* network_layout = column(network, 14, 18);
        network_layout->addWidget(text(QStringLiteral("监听地址和端口"), "heading"));
        auto* fields = new QGridLayout;
        fields->setHorizontalSpacing(12);
        fields->setVerticalSpacing(7);
        address_ = new QLineEdit;
        web_port_ = new QLineEdit;
        signalling_port_ = new QLineEdit;
        web_port_->setReadOnly(true);
        signalling_port_->setReadOnly(true);
        const QList<std::pair<QString, QLineEdit*>> inputs { { QStringLiteral("监听地址"),
                                                                 address_ },
            { QStringLiteral("播放器网页端口"), web_port_ },
            { QStringLiteral("WebRTC 信令端口"), signalling_port_ } };
        for (qsizetype i = 0; i < inputs.size(); ++i) {
            const auto& [label, edit] = inputs[i];
            auto* caption = text(label, "muted");
            caption->setBuddy(edit);
            edit->setAccessibleName(label);
            fields->addWidget(caption, 0, static_cast<int>(i));
            fields->addWidget(edit, 1, static_cast<int>(i));
        }
        fields->setColumnStretch(0, 2);
        fields->setColumnStretch(1, 1);
        fields->setColumnStretch(2, 1);
        network_layout->addLayout(fields);
        description(
            *network_layout, QStringLiteral("两个端口由板端程序定义，此处显示当前默认值。"));
        validation_ = description(*network_layout, { });
        validation_->setVisible(false);
        layout->addWidget(network);

        settings_hint_ = description(*layout, { });
        auto* footer = new QHBoxLayout;
        footer->addStretch();
        cancel_ = new ActionButton({ QStringLiteral("取消修改"), ButtonKind::OUTLINE });
        save_ = new ActionButton({ QStringLiteral("保存配置"), ButtonKind::PRIMARY });
        footer->addWidget(cancel_);
        footer->addWidget(save_);
        layout->addLayout(footer);
        layout->addStretch();
        return scroll;
    }

    auto bind_events() -> void {
        connect(dashboard_nav_, &QPushButton::clicked, &window_,
            [this] { show_page(Page::DASHBOARD); });
        connect(settings_nav_, &QPushButton::clicked, &window_,
            [this] { show_page(Page::SETTINGS); });
        connect(monitor_nav_, &QPushButton::clicked, &window_,
            [this] { show_page(Page::MONITOR); });
        connect(uvc_nav_, &QPushButton::clicked, &window_, [this] { show_page(Page::UVC); });
        connect(rtsp_nav_, &QPushButton::clicked, &window_, [this] { show_page(Page::RTSP); });
        connect(webrtc_nav_, &QPushButton::clicked, &window_, [this] { show_page(Page::WEBRTC); });
        connect(&monitor_, &BoardMonitor::snapshotReceived, monitor_page_,
            &BoardMonitorPage::showSnapshot);
        connect(&monitor_, &BoardMonitor::processesReceived, monitor_page_,
            &BoardMonitorPage::showProcesses);
        connect(&monitor_, &BoardMonitor::stateChanged, monitor_page_,
            &BoardMonitorPage::showStreamState);
        connect(&board_, &BoardClient::statusChanged, monitor_page_,
            &BoardMonitorPage::showServiceStatus);
        connect(refresh_, &QPushButton::clicked, &board_, &BoardClient::refresh);
        connect(theme_, &QPushButton::toggled, &window_, [this](bool dark) {
            if (dark) {
                applyTheme(*qApp, ColorMode::DARK);
                theme_->setText(QStringLiteral("浅色"));
            } else {
                applyTheme(*qApp, ColorMode::LIGHT);
                theme_->setText(QStringLiteral("深色"));
            }
        });
        connect(service_button_, &QPushButton::clicked, &window_, [this] {
            if (status_.running()) {
                board_.operate(Operation::STOP);
            } else {
                board_.operate(Operation::START);
            }
        });
        connect(
            logs_, &QPushButton::clicked, &window_, [this] { board_.operate(Operation::LOGS); });
        connect(enabled_, &QAbstractButton::toggled, &window_, [this] { edit_draft(); });
        connect(address_, &QLineEdit::textEdited, &window_, [this] { edit_draft(); });
        connect(cancel_, &QPushButton::clicked, &window_, [this] {
            reset_draft();
            update_settings();
        });
        connect(save_, &QPushButton::clicked, &window_, [this] { save_settings(); });
        connect(&board_, &BoardClient::statusChanged, &window_, [this](const BoardStatus& state) {
            if (status_.load_state.isEmpty() && state.connected) {
                if (demo_) {
                    event_->setText(QStringLiteral("本地演示已就绪。"));
                } else {
                    event_->setText(QStringLiteral("已读取板端服务状态。"));
                }
            }
            status_ = state;
            if (demo_) {
                monitor_.setDemoMainPid(state.main_pid);
            }
            if (state.connected) {
                connection_detail_.clear();
            }
            if (!dirty_ && status_.configAvailable()) {
                reset_draft();
            }
            update_view();
        });
        connect(&board_, &BoardClient::connectionError, &window_, [this](const QString& detail) {
            connection_detail_ = detail;
            update_view();
        });
        connect(&board_, &BoardClient::busyChanged, &window_, [this](bool busy) {
            busy_ = busy;
            update_view();
        });
        connect(&board_, &BoardClient::operationFinished, &window_,
            [this](Operation operation, bool ok, const QString& detail) {
                if (operation == Operation::LOGS && ok) {
                    open_logs(detail);
                    return;
                }
                if (!ok) {
                    event_->setText(detail);
                    form_feedback_ = detail;
                    update_settings();
                    return;
                }
                event_->setText(operation_label(operation));
                if (operation == Operation::SAVE || operation == Operation::APPLY) {
                    dirty_ = false;
                    form_feedback_ = operation_label(operation);
                }
            });
    }

    auto show_page(Page page) -> void {
        dashboard_nav_->setChecked(page == Page::DASHBOARD);
        settings_nav_->setChecked(page == Page::SETTINGS);
        monitor_nav_->setChecked(page == Page::MONITOR);
        uvc_nav_->setChecked(page == Page::UVC);
        rtsp_nav_->setChecked(page == Page::RTSP);
        webrtc_nav_->setChecked(page == Page::WEBRTC);
        switch (page) {
        case Page::DASHBOARD:
        case Page::SETTINGS:
        case Page::MONITOR:
            if (stack_->currentWidget() == video_page_) {
                video_page_->closePreview();
            }
            stack_->setCurrentIndex(static_cast<int>(page));
            return;
        case Page::UVC:
            stack_->setCurrentWidget(video_page_);
            video_page_->open(PreviewKind::UVC);
            return;
        case Page::RTSP:
            stack_->setCurrentWidget(video_page_);
            video_page_->open(PreviewKind::RTSP);
            return;
        case Page::WEBRTC:
            stack_->setCurrentWidget(video_page_);
            video_page_->open(PreviewKind::WEBRTC);
            return;
        }
    }

    auto update_view() -> void {
        if (status_.connected) {
            link_->setStatus(QStringLiteral("板卡已连接"), StatusTone::GOOD);
        } else if (connection_detail_.isEmpty()) {
            link_->setStatus(QStringLiteral("正在连接"), StatusTone::NEUTRAL);
        } else {
            link_->setStatus(QStringLiteral("板卡离线"), StatusTone::ERROR);
        }

        auto current = service_label(status_);
        if (!status_.connected && !status_.load_state.isEmpty()) {
            current = QStringLiteral("上次状态 · ") + current;
        }
        service_->setText(current);
        QString hint = QStringLiteral("启动后按板端配置提供视频输出。");
        if (status_.running()) {
            hint = QStringLiteral("Live2D 进程已运行，各路输出状态见下方。");
        }
        if (status_.changing() || busy_) {
            hint = QStringLiteral("等待板端操作和状态回读。");
        }
        if (status_.service_state == QStringLiteral("failed")) {
            hint = QStringLiteral("服务失败（%1），可查看运行日志。").arg(status_.service_result);
        }
        if (!connection_detail_.isEmpty()) {
            hint = connection_detail_;
        }
        hero_hint_->setText(hint);
        service_button_->setText(QStringLiteral("启动 Live2D"));
        if (status_.running()) {
            service_button_->setText(QStringLiteral("停止 Live2D"));
        }
        service_button_->setEnabled(status_.connected
            && status_.load_state == QStringLiteral("loaded") && !busy_ && !status_.changing());

        video_page_->setBoardStatus(status_);
        logs_->setEnabled(status_.connected && !busy_);
        refresh_->setEnabled(!busy_);
        update_settings();
    }

    auto valid_address() const -> bool {
        QHostAddress parsed;
        const auto value = address_->text().trimmed();
        return !value.contains('%') && parsed.setAddress(value);
    }

    auto update_settings() -> void {
        QString applied = QStringLiteral("已保存配置 · 等待读取");
        if (status_.configAvailable()) {
            if (status_.webrtc_enabled) {
                applied = QStringLiteral("已保存配置 · WebRTC 已启用");
            } else {
                applied = QStringLiteral("已保存配置 · WebRTC 已关闭");
            }
        }
        applied_->setText(applied);
        if (status_.configAvailable()) {
            web_port_->setText(QString::number(status_.web_port));
            signalling_port_->setText(QString::number(status_.signalling_port));
        }
        const bool available
            = status_.connected && status_.configAvailable() && !busy_ && !status_.changing();
        enabled_->setEnabled(available);
        address_->setEnabled(available && enabled_->isChecked());
        cancel_->setEnabled(dirty_ && !busy_ && status_.configAvailable());
        draft_mark_->setVisible(dirty_);

        const bool invalid = dirty_ && !valid_address();
        if (address_->property("invalid").toBool() != invalid) {
            address_->setProperty("invalid", invalid);
            repolish(*address_);
        }
        const bool stale = dirty_ && draft_revision_ != status_.revision;
        QString note = form_feedback_;
        if (!status_.config_error.isEmpty()) {
            note = status_.config_error;
        } else if (stale) {
            note = QStringLiteral("板端配置已变化。请取消草稿并基于最新配置重新编辑。");
        } else if (invalid) {
            note = QStringLiteral("请输入有效的 IPv4 或 IPv6 监听地址。");
        }
        validation_->setText(note);
        validation_->setVisible(!note.isEmpty());
        save_->setEnabled(available && dirty_ && !invalid && !stale);
        if (status_.running()) {
            save_->setText(QStringLiteral("应用并重启"));
            settings_hint_->setText(
                QStringLiteral("运行中应用配置会重启 Live2D，UVC 和 RTSP 会短暂中断。"));
        } else {
            save_->setText(QStringLiteral("保存配置"));
            settings_hint_->setText(QStringLiteral("保存后在下一次启动 Live2D 时生效。"));
        }
    }

    auto reset_draft() -> void {
        if (!status_.configAvailable()) {
            return;
        }
        syncing_ = true;
        dirty_ = false;
        draft_revision_ = status_.revision;
        enabled_->setChecked(status_.webrtc_enabled);
        if (address_->text() != status_.bind_address) {
            address_->setText(status_.bind_address);
        }
        syncing_ = false;
    }

    auto edit_draft() -> void {
        if (syncing_) {
            return;
        }
        dirty_ = (enabled_->isChecked() != status_.webrtc_enabled)
            || (address_->text().trimmed() != status_.bind_address);
        form_feedback_.clear();
        update_settings();
    }

    auto save_settings() -> void {
        if (!save_->isEnabled()) {
            return;
        }
        auto action = Operation::SAVE;
        if (status_.running()) {
            const auto confirmed
                = QMessageBox::question(&window_, QStringLiteral("应用设置并重启？"),
                    QStringLiteral("Live2D 将重启。UVC 和 RTSP 会在重启期间短暂中断。"),
                    QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
            if (confirmed != QMessageBox::Yes) {
                return;
            }
            action = Operation::APPLY;
        }
        const QJsonObject settings { { QStringLiteral("enabled"), enabled_->isChecked() },
            { QStringLiteral("bindAddress"), address_->text().trimmed() },
            { QStringLiteral("revision"), draft_revision_ } };
        form_feedback_.clear();
        board_.operate(action, settings);
    }

    auto open_logs(const QString& contents) -> void {
        auto* window = new QDialog(&window_);
        window->setAttribute(Qt::WA_DeleteOnClose);
        window->setWindowTitle(QStringLiteral("Live2D 运行日志"));
        window->resize(730, 420);
        auto* layout = column(window, 12, 18);
        layout->addWidget(text(QStringLiteral("vtb-live2d.service"), "heading"));
        auto* viewer = new QPlainTextEdit;
        viewer->setReadOnly(true);
        viewer->setPlainText(contents);
        layout->addWidget(viewer, 1);
        auto* close = new ActionButton({ QStringLiteral("关闭"), ButtonKind::OUTLINE });
        connect(close, &QPushButton::clicked, window, &QDialog::close);
        layout->addWidget(close, 0, Qt::AlignRight);
        window->show();
    }

    MainWindow& window_;
    bool demo_;
    BoardClient board_;
    BoardMonitor monitor_;
    BoardStatus status_;
    bool busy_ = false;
    bool syncing_ = false;
    bool dirty_ = false;
    QString draft_revision_;
    QString connection_detail_;
    QString form_feedback_;

    QStackedWidget* stack_ = nullptr;
    StatusBadge* link_ = nullptr;
    ActionButton* theme_ = nullptr;
    ActionButton* dashboard_nav_ = nullptr;
    ActionButton* settings_nav_ = nullptr;
    ActionButton* monitor_nav_ = nullptr;
    BoardMonitorPage* monitor_page_ = nullptr;
    ActionButton* uvc_nav_ = nullptr;
    ActionButton* rtsp_nav_ = nullptr;
    ActionButton* webrtc_nav_ = nullptr;
    VideoPreview* video_page_ = nullptr;
    ActionButton* refresh_ = nullptr;
    QLabel* service_ = nullptr;
    QLabel* hero_hint_ = nullptr;
    ActionButton* service_button_ = nullptr;
    QLabel* event_ = nullptr;
    ActionButton* logs_ = nullptr;
    StatusSwitch* enabled_ = nullptr;
    QLabel* applied_ = nullptr;
    StatusBadge* draft_mark_ = nullptr;
    QLineEdit* address_ = nullptr;
    QLineEdit* web_port_ = nullptr;
    QLineEdit* signalling_port_ = nullptr;
    QLabel* validation_ = nullptr;
    QLabel* settings_hint_ = nullptr;
    ActionButton* cancel_ = nullptr;
    ActionButton* save_ = nullptr;
};

MainWindow::MainWindow(bool demo)
    : impl_(std::make_unique<Impl>(*this, demo)) { }

MainWindow::~MainWindow() = default;

auto MainWindow::closeEvent(QCloseEvent* event) -> void {
    if (impl_->busy()) {
        event->ignore();
        return;
    }
    QMainWindow::closeEvent(event);
}

} // namespace vtb::desktop
