#include "webrtc_receiver.hh"

#include "av_frame_image.hh"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <rtc/rtc.hpp>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <atomic>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace vtb::desktop {

class WebRtcReceiver::Session final : public std::enable_shared_from_this<Session> {
public:
    Session() : socket_(std::make_shared<rtc::WebSocket>()) {
        codec_ = avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_H264));
        frame_ = av_frame_alloc();
        packet_ = av_packet_alloc();
    }

    ~Session() {
        av_packet_free(&packet_);
        av_frame_free(&frame_);
        avcodec_free_context(&codec_);
    }
    Session(const Session&) = delete;
    auto operator=(const Session&) -> Session& = delete;
    Session(Session&&) = delete;
    auto operator=(Session&&) -> Session& = delete;

    auto open(const QUrl& url) -> void {
        if (!codec_ || !frame_ || !packet_) {
            throw std::runtime_error("cannot allocate the WebRTC H.264 decoder");
        }
        codec_->thread_count = 2;
        codec_->thread_type = FF_THREAD_SLICE;
        if (avcodec_open2(codec_, avcodec_find_decoder(AV_CODEC_ID_H264), nullptr) < 0) {
            throw std::runtime_error("cannot open the WebRTC H.264 decoder");
        }
        const auto weak = weak_from_this();
        socket_->onMessage([weak](rtc::message_variant value) {
            const auto* message = std::get_if<std::string>(&value);
            if (const auto session = weak.lock(); session && message && !session->stopped_.load()) {
                try {
                    session->handleMessage(QJsonDocument::fromJson(
                        QByteArray::fromStdString(*message)).object());
                } catch (const std::exception& error) {
                    session->fail(QString::fromUtf8(error.what()));
                }
            }
        });
        socket_->onError([weak](std::string error) {
            if (const auto session = weak.lock()) {
                session->fail(QString::fromStdString(error));
            }
        });
        socket_->onClosed([weak] {
            if (const auto session = weak.lock()) {
                session->fail(QStringLiteral("WebRTC 信令连接已关闭。"));
            }
        });
        socket_->open(url.toString(QUrl::FullyEncoded).toStdString());
    }

    auto stop() -> void {
        stopped_.store(true);
        std::shared_ptr<rtc::PeerConnection> peer;
        {
            const std::scoped_lock lock(signalling_);
            peer = std::move(peer_);
        }
        if (peer) {
            peer->close();
        }
        socket_->close();
    }

    auto takeFrame() -> QImage {
        const std::scoped_lock lock(output_);
        return std::exchange(latest_, {});
    }

    auto error() -> QString {
        const std::scoped_lock lock(output_);
        return error_;
    }

private:
    auto send(const QJsonObject& message) -> void {
        if (!stopped_.load() && socket_->isOpen()) {
            socket_->send(QJsonDocument(message).toJson(QJsonDocument::Compact).toStdString());
        }
    }

    auto fail(const QString& detail) -> void {
        const std::scoped_lock lock(output_);
        if (!stopped_.load() && error_.isEmpty()) {
            error_ = detail;
        }
    }

    auto handleMessage(const QJsonObject& value) -> void {
        const std::scoped_lock lock(signalling_);
        if (stopped_.load()) {
            return;
        }
        const auto type = value.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("welcome")) {
            send({ { "type", "setPeerStatus" }, { "roles", QJsonArray{ "listener" } } });
            send({ { "type", "list" } });
        } else if (type == QStringLiteral("list")) {
            for (const auto& item : value.value(QStringLiteral("producers")).toArray()) {
                const auto producer = item.toObject();
                subscribe(producer, producer.value(QStringLiteral("id")).toString());
            }
        } else if (type == QStringLiteral("peerStatusChanged")) {
            if (value.value(QStringLiteral("roles")).toArray().contains(QStringLiteral("producer"))) {
                subscribe(value, value.value(QStringLiteral("peerId")).toString());
            }
        } else if (type == QStringLiteral("sessionStarted")) {
            session_id_ = value.value(QStringLiteral("sessionId")).toString();
        } else if (type == QStringLiteral("peer")) {
            handlePeer(value);
        } else if (type == QStringLiteral("error") || type == QStringLiteral("endSession")) {
            auto message = value.value(QStringLiteral("details")).toString();
            if (message.isEmpty()) {
                message = QStringLiteral("WebRTC 会话已结束。");
            }
            fail(message);
        }
    }

    auto subscribe(const QJsonObject& producer, const QString& id) -> void {
        const auto name = producer.value(QStringLiteral("meta")).toObject()
            .value(QStringLiteral("name")).toString();
        if (!subscribed_ && !id.isEmpty() && name == QStringLiteral("live2d")) {
            subscribed_ = true;
            send({ { "type", "startSession" }, { "peerId", id } });
        }
    }

    auto handlePeer(const QJsonObject& value) -> void {
        const auto id = value.value(QStringLiteral("sessionId")).toString();
        if (id.isEmpty() || id != session_id_) {
            return;
        }
        const auto sdp = value.value(QStringLiteral("sdp")).toObject();
        if (sdp.value(QStringLiteral("type")).toString() == QStringLiteral("offer") && !peer_) {
            rtc::Description offer(
                sdp.value(QStringLiteral("sdp")).toString().toStdString(), "offer");
            configurePeer(id, offer);
            peer_->setRemoteDescription(std::move(offer));
            peer_->setLocalDescription(rtc::Description::Type::Answer);
            for (auto& pending : remote_candidates_) {
                peer_->addRemoteCandidate(std::move(pending));
            }
            remote_candidates_.clear();
        }
        const auto ice = value.value(QStringLiteral("ice")).toObject();
        const auto candidate = ice.value(QStringLiteral("candidate")).toString();
        if (!candidate.isEmpty()) {
            auto mid = ice.value(QStringLiteral("sdpMid")).toString();
            if (mid.isEmpty()) {
                mid = QStringLiteral("video0");
            }
            rtc::Candidate remote(candidate.toStdString(), mid.toStdString());
            if (peer_) {
                peer_->addRemoteCandidate(std::move(remote));
            } else {
                remote_candidates_.push_back(std::move(remote));
            }
        }
    }

    auto configurePeer(const QString& id, rtc::Description& offer) -> void {
        rtc::Configuration configuration;
        configuration.disableAutoNegotiation = true;
        peer_ = std::make_shared<rtc::PeerConnection>(configuration);
        const auto weak = weak_from_this();
        peer_->onLocalDescription([weak, id](rtc::Description description) {
            if (const auto session = weak.lock()) {
                session->send({ { "type", "peer" }, { "sessionId", id },
                    { "sdp", QJsonObject{ { "type", QString::fromStdString(description.typeString()) },
                                 { "sdp", QString::fromStdString(std::string(description)) } } } });
            }
        });
        peer_->onLocalCandidate([weak, id](rtc::Candidate candidate) {
            if (const auto session = weak.lock()) {
                session->send({ { "type", "peer" }, { "sessionId", id },
                    { "ice", QJsonObject{ { "candidate", QString::fromStdString(candidate.candidate()) },
                                 { "sdpMid", QString::fromStdString(candidate.mid()) },
                                 { "sdpMLineIndex", 0 } } } });
            }
        });
        peer_->onStateChange([weak](rtc::PeerConnection::State state) {
            if (state == rtc::PeerConnection::State::Failed) {
                if (const auto session = weak.lock()) {
                    session->fail(QStringLiteral("WebRTC 媒体连接失败。"));
                }
            }
        });
        for (int index = 0; index < offer.mediaCount(); ++index) {
            const auto offered = offer.media(index);
            const auto* video = std::get_if<rtc::Description::Media*>(&offered);
            if (!video || (*video)->type() != "video") {
                continue;
            }
            // The decoder accepts H.264 RTP. Leave redundancy and repair payloads unnegotiated.
            auto media = (*video)->reciprocate();
            for (const int payload : media.payloadTypes()) {
                // Removing a payload also removes its associated RTX entry.
                if (media.hasPayloadType(payload) && media.rtpMap(payload)->format != "H264") {
                    media.removeRtpMap(payload);
                }
            }
            if (media.payloadTypes().empty()) {
                continue;
            }
            const auto track = peer_->addTrack(std::move(media));
            track_ = track;
            auto depacketizer = std::make_shared<rtc::H264RtpDepacketizer>();
            // Incoming handlers run in reverse order, so RTCP sees RTP before depacketization.
            depacketizer->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
            track->setMediaHandler(depacketizer);
            track->onOpen([weak_track = std::weak_ptr(track)] {
                if (const auto opened = weak_track.lock()) {
                    opened->requestKeyframe();
                }
            });
            track->onFrame([weak](rtc::binary data, rtc::FrameInfo) {
                if (const auto session = weak.lock(); session && !session->stopped_.load()) {
                    session->decode(data);
                }
            });
            return;
        }
        throw std::runtime_error("WebRTC offer has no H.264 video track");
    }

    auto decode(const rtc::binary& data) -> void {
        const std::scoped_lock lock(decoding_);
        if (data.empty() || data.size() > 16 * 1024 * 1024) {
            return;
        }
        if (av_new_packet(packet_, static_cast<int>(data.size())) < 0) {
            return;
        }
        std::memcpy(packet_->data, data.data(), data.size());
        const auto accepted = avcodec_send_packet(codec_, packet_);
        av_packet_unref(packet_);
        if (accepted < 0) {
            return;
        }
        while (avcodec_receive_frame(codec_, frame_) >= 0) {
            auto image = converter_.convert(*frame_);
            if (!image.isNull()) {
                const std::scoped_lock guard(output_);
                latest_ = std::move(image);
            }
        }
    }

    std::atomic<bool> stopped_{ false };
    const std::shared_ptr<rtc::WebSocket> socket_;
    std::mutex signalling_;
    std::shared_ptr<rtc::PeerConnection> peer_;
    std::shared_ptr<rtc::Track> track_;
    bool subscribed_ = false;
    QString session_id_;
    std::vector<rtc::Candidate> remote_candidates_;
    std::mutex decoding_;
    AVCodecContext* codec_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    AvFrameImage converter_;
    std::mutex output_;
    QImage latest_;
    QString error_;
};

WebRtcReceiver::~WebRtcReceiver() { stop(); }

auto WebRtcReceiver::start(const QUrl& url) -> void {
    stop();
    try {
        session_ = std::make_shared<Session>();
        session_->open(url);
    } catch (const std::exception& error) {
        stop();
        startup_error_ = QString::fromUtf8(error.what());
    }
}

auto WebRtcReceiver::stop() -> void {
    startup_error_.clear();
    if (session_) {
        session_->stop();
        session_.reset();
    }
}

auto WebRtcReceiver::takeFrame() -> QImage {
    if (session_) {
        return session_->takeFrame();
    }
    return {};
}

auto WebRtcReceiver::error() -> QString {
    if (session_) {
        return session_->error();
    }
    return startup_error_;
}

} // namespace vtb::desktop
