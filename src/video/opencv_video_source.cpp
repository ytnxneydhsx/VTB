#include "vtb/video/opencv_video_source.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
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

bool is_decimal_device_index(const std::string& source) noexcept {
    if (source.empty()) {
        return false;
    }
    return std::all_of(
        source.begin(), source.end(), [](unsigned char value) {
            return std::isdigit(value) != 0;
        });
}

bool is_live_camera_source(const std::string& source) noexcept {
    if (is_decimal_device_index(source)) {
        return true;
    }
    return source.rfind("/dev/video", 0) == 0;
}

std::uint64_t monotonic_now_ns() {
    timespec timestamp{};
    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0) {
        throw std::runtime_error("clock_gettime(CLOCK_MONOTONIC) failed");
    }
    return static_cast<std::uint64_t>(timestamp.tv_sec) * 1000000000ULL +
        static_cast<std::uint64_t>(timestamp.tv_nsec);
}

}  // namespace

class OpenCvVideoSourceState final {
public:
    explicit OpenCvVideoSourceState(OpenCvVideoSourceConfig config)
        : config_(std::move(config)), live_camera_(is_live_camera_source(config_.source)) {
        if (config_.source.empty() || config_.width == 0 || config_.height == 0 ||
            config_.fps == 0 || config_.buffer_count < 2) {
            throw std::invalid_argument("OpenCV video source configuration is invalid");
        }
        bool opened = false;
        if (is_decimal_device_index(config_.source)) {
            opened = capture_.open(std::stoi(config_.source), cv::CAP_V4L2);
        } else if (live_camera_) {
            opened = capture_.open(config_.source, cv::CAP_V4L2);
        } else {
            opened = capture_.open(config_.source);
        }
        if (!opened) {
            throw std::runtime_error(
                "Unable to open OpenCV video source: " + config_.source);
        }
        if (live_camera_) {
            capture_.set(cv::CAP_PROP_BUFFERSIZE, config_.buffer_count);
            if (config_.request_mjpeg) {
                capture_.set(
                    cv::CAP_PROP_FOURCC,
                    cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
            }
            capture_.set(cv::CAP_PROP_FRAME_WIDTH, config_.width);
            capture_.set(cv::CAP_PROP_FRAME_HEIGHT, config_.height);
            capture_.set(cv::CAP_PROP_FPS, config_.fps);
        }
    }

    ~OpenCvVideoSourceState() {
        close();
    }

    OpenCvVideoSourceState(const OpenCvVideoSourceState&) = delete;
    OpenCvVideoSourceState& operator=(const OpenCvVideoSourceState&) = delete;
    OpenCvVideoSourceState(OpenCvVideoSourceState&&) = delete;
    OpenCvVideoSourceState& operator=(OpenCvVideoSourceState&&) = delete;

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
    bool live_camera_ = false;
};

OpenCvVideoSource::OpenCvVideoSource(OpenCvVideoSourceConfig config)
    : state_(std::make_unique<OpenCvVideoSourceState>(std::move(config))) {}

OpenCvVideoSource::~OpenCvVideoSource() = default;

std::optional<DecodedFrameLease> OpenCvVideoSource::read() {
    if (!state_) {
        return std::nullopt;
    }
    return state_->read();
}

void OpenCvVideoSource::request_stop() noexcept {
    if (state_) {
        state_->request_stop();
    }
}

void OpenCvVideoSource::close() noexcept {
    if (state_) {
        state_->close();
    }
}

}  // namespace vtb
