#pragma once

#include "board_client.hh"
#include "mac_preview.hh"
#include "rtsp_receiver.hh"
#include "webrtc_receiver.hh"
#include "widgets.hh"

#include <QElapsedTimer>
#include <QWidget>

class QComboBox;
class QStackedWidget;

namespace vtb::desktop {

enum class PreviewKind { UVC, RTSP, WEBRTC };

class VideoPreview final : public QWidget {
public:
    explicit VideoPreview(bool demo, QWidget* parent = nullptr);
    ~VideoPreview() override;
    VideoPreview(const VideoPreview&) = delete;
    auto operator=(const VideoPreview&) -> VideoPreview& = delete;
    VideoPreview(VideoPreview&&) = delete;
    auto operator=(VideoPreview&&) -> VideoPreview& = delete;

    auto open(PreviewKind kind) -> void;
    auto closePreview() -> void;
    auto setBoardStatus(const BoardStatus& status) -> void;

private:
    class FrameView;

    auto refresh(bool user_requested) -> void;
    auto refreshCameras(bool user_requested) -> void;
    auto startNetwork(const QUrl& url) -> void;
    auto streamUrl() const -> QUrl;
    auto tick() -> void;
    auto stopPlayback() -> void;
    auto waiting(const QString& label, const QString& detail, StatusTone tone) -> void;
    auto displayedFrame() -> void;

    const bool demo_;
    bool active_ = false;
    bool permission_pending_ = false;
    PreviewKind kind_ = PreviewKind::UVC;
    BoardStatus board_;
    QUrl playing_url_;
    QString camera_id_;
    quint64 last_native_frames_ = 0;
    QElapsedTimer last_frame_;
    QTimer display_;
    QTimer source_poll_;
    QTimer retry_;

    QLabel* title_ = nullptr;
    QLabel* source_ = nullptr;
    QLabel* detail_ = nullptr;
    QLabel* placeholder_ = nullptr;
    StatusBadge* badge_ = nullptr;
    QComboBox* cameras_ = nullptr;
    ActionButton* reconnect_ = nullptr;
    QStackedWidget* video_ = nullptr;
    FrameView* frame_ = nullptr;
    QWidget* native_host_ = nullptr;
    std::unique_ptr<MacPreview> mac_;
    RtspReceiver rtsp_;
    WebRtcReceiver webrtc_;
};

} // namespace vtb::desktop
