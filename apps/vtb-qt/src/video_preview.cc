#include "video_preview.hh"

#include "theme.hh"

#include <QComboBox>
#include <QFrame>
#include <QPainter>
#include <QPointer>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace vtb::desktop {
namespace {

    auto label(const QString& content, const char* role = "") -> QLabel* {
        auto* widget = new QLabel(content);
        widget->setProperty("role", role);
        widget->setTextFormat(Qt::PlainText);
        return widget;
    }

    auto name(PreviewKind kind) -> QString {
        switch (kind) {
        case PreviewKind::UVC: return QStringLiteral("UVC");
        case PreviewKind::RTSP: return QStringLiteral("RTSP");
        case PreviewKind::WEBRTC: return QStringLiteral("WebRTC");
        }
        Q_UNREACHABLE();
    }

} // namespace

class VideoPreview::FrameView final : public QWidget {
public:
    auto setFrame(QImage frame) -> void {
        frame_ = std::move(frame);
        update();
    }

protected:
    auto paintEvent(QPaintEvent*) -> void override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(22, 27, 36));
        if (frame_.isNull()) {
            return;
        }
        const auto fitted = frame_.size().scaled(size(), Qt::KeepAspectRatio);
        const QPoint origin((width() - fitted.width()) / 2, (height() - fitted.height()) / 2);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(QRect(origin, fitted), frame_);
    }

private:
    QImage frame_;
};

VideoPreview::VideoPreview(bool demo, QWidget* parent)
    : QWidget(parent)
    , demo_(demo) {
    setObjectName(QStringLiteral("page"));
    auto* layout = new QVBoxLayout(this);
    const auto margin = metrics::kContentMargin;
    layout->setContentsMargins(margin, margin, margin, margin);
    layout->setSpacing(metrics::kSpacing);

    auto* heading = new QHBoxLayout;
    title_ = label({}, "title");
    badge_ = new StatusBadge;
    badge_->setProperty("previewState", true);
    reconnect_ = new ActionButton({ QStringLiteral("重新连接"), ButtonKind::TONAL });
    heading->addWidget(title_);
    heading->addStretch();
    heading->addWidget(badge_);
    heading->addWidget(reconnect_);
    layout->addLayout(heading);

    auto* source_card = new QFrame;
    source_card->setProperty("panel", "surface");
    auto* source_layout = new QHBoxLayout(source_card);
    source_layout->setContentsMargins(16, 6, 16, 6);
    source_layout->addWidget(label(QStringLiteral("来源"), "muted"));
    source_ = label({}, "sub");
    source_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    cameras_ = new QComboBox;
    cameras_->setAccessibleName(QStringLiteral("UVC 摄像头"));
    cameras_->setEnabled(!demo_);
    source_layout->addWidget(source_, 1);
    source_layout->addWidget(cameras_, 1);
    layout->addWidget(source_card);

    video_ = new QStackedWidget;
    video_->setMinimumHeight(230);
    video_->setObjectName(QStringLiteral("videoPreviewSurface"));
    placeholder_ = label({});
    placeholder_->setAlignment(Qt::AlignCenter);
    placeholder_->setWordWrap(true);
    placeholder_->setObjectName(QStringLiteral("videoPreviewPlaceholder"));
    frame_ = new FrameView;
    native_host_ = new QWidget;
    video_->addWidget(placeholder_);
    video_->addWidget(frame_);
    video_->addWidget(native_host_);
    mac_ = std::make_unique<MacPreview>(*native_host_);
    layout->addWidget(video_, 1);

    detail_ = label({}, "muted");
    detail_->setWordWrap(true);
    layout->addWidget(detail_);

    display_.setTimerType(Qt::PreciseTimer);
    display_.setInterval(16);
    source_poll_.setInterval(1000);
    retry_.setSingleShot(true);
    connect(&display_, &QTimer::timeout, this, &VideoPreview::tick);
    connect(&source_poll_, &QTimer::timeout, this, [this] { refreshCameras(false); });
    connect(&retry_, &QTimer::timeout, this, [this] { refresh(true); });
    connect(reconnect_, &QPushButton::clicked, this, [this] { refresh(true); });
    connect(cameras_, &QComboBox::activated, this, [this] { refreshCameras(true); });
}

VideoPreview::~VideoPreview() { closePreview(); }

auto VideoPreview::open(PreviewKind kind) -> void {
    if (active_ && kind == kind_) {
        return;
    }
    closePreview();
    active_ = true;
    kind_ = kind;
    title_->setText(QStringLiteral("%1 预览").arg(name(kind)));
    cameras_->setVisible(kind == PreviewKind::UVC);
    source_->setVisible(kind != PreviewKind::UVC);
    reconnect_->setEnabled(!demo_);
    if (demo_) {
        source_->setText(streamUrl().toDisplayString(QUrl::RemovePassword));
        if (kind == PreviewKind::UVC && cameras_->count() == 0) {
            cameras_->addItem(QStringLiteral("RK3588 Live2D Composite"));
        }
        waiting(QStringLiteral("示例数据"), QStringLiteral("连接 RK3588 后显示实际画面。"),
            StatusTone::NEUTRAL);
        return;
    }
    refresh(false);
    display_.start();
    if (kind_ == PreviewKind::UVC) {
        source_poll_.start();
    }
}

auto VideoPreview::closePreview() -> void {
    active_ = false;
    display_.stop();
    source_poll_.stop();
    stopPlayback();
}

auto VideoPreview::setBoardStatus(const BoardStatus& status) -> void {
    board_ = status;
    if (active_ && !demo_ && kind_ != PreviewKind::UVC) {
        refresh(false);
    }
}

auto VideoPreview::streamUrl() const -> QUrl {
    if (kind_ == PreviewKind::RTSP) {
        const QUrl url(board_.rtsp_url);
        if (url.scheme() == QStringLiteral("rtsp") && !url.host().isEmpty()) {
            return url;
        }
        return {};
    }
    QUrl url;
    auto host = board_.bind_address;
    if (host == QStringLiteral("0.0.0.0") || host == QStringLiteral("::")) {
        host = QStringLiteral("192.168.77.2");
    }
    if (!host.isEmpty()) {
        url.setScheme(QStringLiteral("ws"));
        url.setHost(host);
        url.setPort(board_.signalling_port);
        url.setPath(QStringLiteral("/"));
    }
    return url;
}

auto VideoPreview::refresh(bool user_requested) -> void {
    if (kind_ == PreviewKind::UVC) {
        refreshCameras(user_requested);
        return;
    }
    const auto url = streamUrl();
    source_->setText(url.toDisplayString(QUrl::RemovePassword));
    if (!board_.connected) {
        if (playing_url_.isEmpty()) {
            waiting(QStringLiteral("等待板卡"), QStringLiteral("正在读取板端播放地址。"),
                StatusTone::NEUTRAL);
        }
        return;
    }
    bool available = board_.webRtcPreviewAvailable();
    if (kind_ == PreviewKind::RTSP) {
        available = board_.running() && board_.rtsp_enabled && board_.rtsp_listening;
    }
    if (!available || !url.isValid() || url.isEmpty()) {
        stopPlayback();
        waiting(QStringLiteral("等待输出"), QStringLiteral("请确认 Live2D 已启动且该路输出已就绪。"),
            StatusTone::WARNING);
        return;
    }
    if (user_requested || url != playing_url_) {
        startNetwork(url);
    }
}

auto VideoPreview::refreshCameras(bool user_requested) -> void {
    const auto list = MacPreview::cameras();
    auto chosen = cameras_->currentData().toString();
    bool changed = cameras_->count() != list.size() + 1;
    for (qsizetype i = 0; !changed && i < list.size(); ++i) {
        const auto index = static_cast<int>(i) + 1;
        changed = cameras_->itemData(index).toString() != list[i].id;
    }
    if (changed) {
        const QSignalBlocker block(cameras_);
        cameras_->clear();
        cameras_->addItem(QStringLiteral("选择摄像头"), QString());
        int preferred = 0;
        for (const auto& camera : list) {
            cameras_->addItem(camera.name, camera.id);
            if (camera.name.contains(QStringLiteral("Live2D"), Qt::CaseInsensitive)
                || camera.name.contains(QStringLiteral("VTB"), Qt::CaseInsensitive)) {
                preferred = cameras_->count() - 1;
            }
        }
        const auto previous = cameras_->findData(chosen);
        if (chosen.isEmpty() || previous < 0) {
            cameras_->setCurrentIndex(preferred);
        } else {
            cameras_->setCurrentIndex(previous);
        }
        chosen = cameras_->currentData().toString();
    }
    if (chosen.isEmpty()) {
        if (!camera_id_.isEmpty()) {
            stopPlayback();
        }
        auto hint = QStringLiteral("连接 RK3588 的 USB 数据线，等待 Mac 识别摄像头。");
        if (board_.connected && !board_.uvc_enabled) {
            hint = QStringLiteral("板端当前关闭了 UVC 输出。");
        } else if (board_.uvc_host_state == QStringLiteral("configured")) {
            hint = QStringLiteral("板端已完成 USB 枚举。请在 Mac 的设备列表中选择 RK3588。");
        }
        waiting(QStringLiteral("等待 UVC 设备"), hint, StatusTone::WARNING);
        return;
    }
    const auto permission = MacPreview::cameraPermission();
    if (permission != CameraPermission::ALLOWED && !camera_id_.isEmpty()) {
        stopPlayback();
    }
    if (permission == CameraPermission::DENIED) {
        waiting(QStringLiteral("相机权限不足"),
            QStringLiteral("在「系统设置 → 隐私与安全性 → 相机」中允许 VTB Qt。"),
            StatusTone::WARNING);
        return;
    }
    if (permission == CameraPermission::UNDETERMINED) {
        waiting(QStringLiteral("等待相机授权"), QStringLiteral("点击「重新连接」请求相机访问。"),
            StatusTone::NEUTRAL);
        if (user_requested && !permission_pending_) {
            permission_pending_ = true;
            const QPointer<VideoPreview> page(this);
            MacPreview::requestCameraPermission([page](bool) {
                if (page) {
                    page->permission_pending_ = false;
                    if (page->active_ && page->kind_ == PreviewKind::UVC) {
                        page->refreshCameras(false);
                    }
                }
            });
        }
        return;
    }
    if (user_requested || chosen != camera_id_) {
        stopPlayback();
        camera_id_ = chosen;
        video_->setCurrentWidget(native_host_);
        badge_->setStatus(QStringLiteral("等待首帧"), StatusTone::NEUTRAL);
        detail_->setText(QStringLiteral("正在打开选中的 UVC 摄像头。"));
        last_frame_.start();
        mac_->startCamera(chosen);
    }
}

auto VideoPreview::startNetwork(const QUrl& url) -> void {
    stopPlayback();
    playing_url_ = url;
    last_frame_.start();
    waiting(QStringLiteral("等待首帧"), QStringLiteral("正在连接 %1。").arg(name(kind_)),
        StatusTone::NEUTRAL);
    if (kind_ == PreviewKind::RTSP) {
        rtsp_.start(url.toString(QUrl::FullyEncoded));
    } else {
        webrtc_.start(url);
    }
}

auto VideoPreview::tick() -> void {
    if (kind_ != PreviewKind::UVC && !playing_url_.isEmpty()) {
        QImage image;
        QString error;
        if (kind_ == PreviewKind::RTSP) {
            image = rtsp_.takeFrame();
            error = rtsp_.error();
        } else {
            image = webrtc_.takeFrame();
            error = webrtc_.error();
        }
        if (!image.isNull()) {
            frame_->setFrame(std::move(image));
            video_->setCurrentWidget(frame_);
            displayedFrame();
        }
        if (!error.isEmpty()) {
            waiting(QStringLiteral("连接中断"), error, StatusTone::WARNING);
            if (!retry_.isActive()) {
                retry_.start(3000);
            }
        } else if (last_frame_.isValid() && last_frame_.elapsed() > 8000) {
            waiting(QStringLiteral("等待画面"), QStringLiteral("长时间未收到新视频帧。"),
                StatusTone::WARNING);
            if (!retry_.isActive()) {
                retry_.start(3000);
            }
        }
        return;
    }
    if (playing_url_.isEmpty() && camera_id_.isEmpty()) {
        return;
    }
    const auto frames = mac_->frames();
    if (frames != last_native_frames_) {
        last_native_frames_ = frames;
        displayedFrame();
    }
    const auto error = mac_->error();
    if (!error.isEmpty()) {
        waiting(QStringLiteral("预览异常"), error, StatusTone::WARNING);
    } else if (last_frame_.isValid() && last_frame_.elapsed() > 7000) {
        badge_->setStatus(QStringLiteral("等待画面"), StatusTone::WARNING);
        detail_->setText(QStringLiteral("长时间未收到新视频帧。"));
    }
}

auto VideoPreview::stopPlayback() -> void {
    retry_.stop();
    rtsp_.stop();
    webrtc_.stop();
    mac_->stop();
    playing_url_.clear();
    camera_id_.clear();
    last_native_frames_ = 0;
    last_frame_.invalidate();
    frame_->setFrame({});
}

auto VideoPreview::waiting(const QString& label, const QString& detail, StatusTone tone) -> void {
    badge_->setStatus(label, tone);
    placeholder_->setText(detail);
    detail_->clear();
    video_->setCurrentWidget(placeholder_);
}

auto VideoPreview::displayedFrame() -> void {
    last_frame_.restart();
    badge_->setStatus(QStringLiteral("播放中"), StatusTone::GOOD);
    detail_->setText(QStringLiteral("切换页面时释放当前预览连接。"));
}

} // namespace vtb::desktop
