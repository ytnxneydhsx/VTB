#pragma once

#include <QImage>

extern "C" {
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

namespace vtb::desktop {

class AvFrameImage final {
public:
    AvFrameImage() = default;
    ~AvFrameImage() { sws_freeContext(scaler_); }
    AvFrameImage(const AvFrameImage&) = delete;
    auto operator=(const AvFrameImage&) -> AvFrameImage& = delete;
    AvFrameImage(AvFrameImage&&) = delete;
    auto operator=(AvFrameImage&&) -> AvFrameImage& = delete;

    auto convert(const AVFrame& frame) -> QImage {
        QImage image(frame.width, frame.height, QImage::Format_RGB32);
        if (image.isNull()) {
            return {};
        }
        scaler_ = sws_getCachedContext(scaler_, frame.width, frame.height,
            static_cast<AVPixelFormat>(frame.format), image.width(), image.height(), AV_PIX_FMT_BGRA,
            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (!scaler_) {
            return {};
        }
        uint8_t* pixels[]{ image.bits() };
        int stride[]{ static_cast<int>(image.bytesPerLine()) };
        if (sws_scale(scaler_, frame.data, frame.linesize, 0, frame.height, pixels, stride)
            != frame.height) {
            return {};
        }
        return image;
    }

private:
    SwsContext* scaler_ = nullptr;
};

} // namespace vtb::desktop
