#include "vtb/video/opencv_video_source.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <time.h>
#include <utility>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

namespace vtb {
namespace {

std::uint64_t monotonic_now_ns() {
    timespec timestamp{};
    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0) {
        throw std::runtime_error("clock_gettime(CLOCK_MONOTONIC) failed");
    }
    return static_cast<std::uint64_t>(timestamp.tv_sec) * 1000000000ULL +
        static_cast<std::uint64_t>(timestamp.tv_nsec);
}

void set_capture_property(
    cv::VideoCapture& capture,
    int property,
    double value,
    const char* description) {
    if (!capture.set(property, value)) {
        throw std::runtime_error(
            std::string("OpenCV V4L2 backend rejected ") + description);
    }
}

void require_capture_property(
    const cv::VideoCapture& capture,
    int property,
    std::uint32_t expected,
    const char* description) {
    const double raw_value = capture.get(property);
    if (!std::isfinite(raw_value) || raw_value < 0.0 ||
        static_cast<std::uint64_t>(std::llround(raw_value)) != expected) {
        throw std::runtime_error(
            std::string("OpenCV V4L2 backend did not apply ") + description);
    }
}

}  // namespace

class OpenCvVideoSource::Impl final {
public:
    explicit Impl(OpenCvVideoSourceConfig config)
        : config_(std::move(config)) {
        if (config_.device_path.empty() || config_.pixel_format == 0 ||
            config_.width == 0 || config_.height == 0 || config_.fps == 0 ||
            config_.buffer_count < 2) {
            throw std::invalid_argument("OpenCV video source configuration is invalid");
        }
        if (!capture_.open(config_.device_path, cv::CAP_V4L2)) {
            throw std::runtime_error(
                "Unable to open OpenCV V4L2 device: " + config_.device_path);
        }
        set_capture_property(
            capture_, cv::CAP_PROP_FOURCC, config_.pixel_format, "pixel format");
        set_capture_property(
            capture_, cv::CAP_PROP_FRAME_WIDTH, config_.width, "frame width");
        set_capture_property(
            capture_, cv::CAP_PROP_FRAME_HEIGHT, config_.height, "frame height");
        set_capture_property(capture_, cv::CAP_PROP_FPS, config_.fps, "frame rate");
        set_capture_property(
            capture_, cv::CAP_PROP_BUFFERSIZE, config_.buffer_count, "buffer count");
        require_capture_property(
            capture_, cv::CAP_PROP_FOURCC, config_.pixel_format, "pixel format");
        require_capture_property(
            capture_, cv::CAP_PROP_FRAME_WIDTH, config_.width, "frame width");
        require_capture_property(
            capture_, cv::CAP_PROP_FRAME_HEIGHT, config_.height, "frame height");
        require_capture_property(
            capture_, cv::CAP_PROP_FPS, config_.fps, "frame rate");
        require_capture_property(
            capture_, cv::CAP_PROP_BUFFERSIZE, config_.buffer_count, "buffer count");
    }

    ~Impl() {
        close();
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    std::optional<DecodedFrameLease> read() {
        if (stop_requested_.load(std::memory_order_acquire)) {
            return std::nullopt;
        }
        cv::Mat frame;
        if (!capture_.read(frame)) {
            return std::nullopt;
        }
        const std::uint64_t timestamp_ns = monotonic_now_ns();
        if (frame.empty() || frame.type() != CV_8UC3) {
            throw std::runtime_error(
                "OpenCV video source must produce an 8-bit three-channel frame");
        }
        const std::size_t row_bytes =
            static_cast<std::size_t>(frame.cols) * 3U;
        const std::size_t storage_size =
            row_bytes * static_cast<std::size_t>(frame.rows);
        auto storage = std::make_shared<std::vector<std::byte>>(storage_size);
        for (int row = 0; row < frame.rows; ++row) {
            std::memcpy(
                storage->data() + static_cast<std::size_t>(row) * row_bytes,
                frame.ptr(row),
                row_bytes);
        }
        std::array<FramePlaneView, FrameView::max_planes> planes{};
        planes[0] = FramePlaneView(
            storage->data(),
            storage->size(),
            static_cast<std::uint32_t>(row_bytes),
            -1,
            0);
        FrameView view(
            VideoPixelFormat::bgr888,
            FrameMemoryType::cpu,
            static_cast<std::uint32_t>(frame.cols),
            static_cast<std::uint32_t>(frame.rows),
            next_frame_id_++,
            timestamp_ns,
            FrameTimestampSource::dequeue_monotonic,
            planes,
            1);
        return DecodedFrameLease(
            std::move(view), [storage = std::move(storage)]() {});
    }

    void request_stop() noexcept {
        stop_requested_.store(true, std::memory_order_release);
    }

    void close() noexcept {
        request_stop();
        capture_.release();
    }

private:
    OpenCvVideoSourceConfig config_;
    cv::VideoCapture capture_;
    std::atomic<bool> stop_requested_{false};
    std::uint64_t next_frame_id_ = 1;
};

OpenCvVideoSource::OpenCvVideoSource(OpenCvVideoSourceConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

OpenCvVideoSource::~OpenCvVideoSource() = default;

std::optional<DecodedFrameLease> OpenCvVideoSource::read() {
    if (!impl_) {
        return std::nullopt;
    }
    return impl_->read();
}

void OpenCvVideoSource::request_stop() noexcept {
    if (impl_) {
        impl_->request_stop();
    }
}

void OpenCvVideoSource::close() noexcept {
    if (impl_) {
        impl_->close();
    }
}

}  // namespace vtb
