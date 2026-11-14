#include "vtb/video/camera_source.hpp"

#include <memory>
#include <utility>

#include "vtb/video/v4l2/v4l2_buffer_pool.hpp"

namespace vtb {

CameraSource::CameraSource(
    CaptureConfig config,
    std::uint32_t buffer_count) {
    buffer_pool_ = std::make_shared<V4l2BufferPool>(
        std::move(config),
        buffer_count);
}

CameraSource::~CameraSource() {
    close();
}

std::optional<DecodedFrameLease> CameraSource::read() {
    if (!buffer_pool_) {
        return std::nullopt;
    }
    return buffer_pool_->dequeue();
}

void CameraSource::request_stop() noexcept {
    if (buffer_pool_) {
        buffer_pool_->request_stop();
    }
}

void CameraSource::close() noexcept {
    request_stop();
    buffer_pool_.reset();
}

}  // namespace vtb
