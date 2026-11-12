#include "live2d_tracking_session.hpp"

#include <atomic>
#include <cmath>
#include <mutex>
#include <utility>

#include "vtb/face/face_frame_consumer.hpp"
#include "vtb/pipeline/video_processing_runtime.hpp"
#include "vtb/time/monotonic_clock.hpp"

namespace vtb {
namespace {

constexpr float kViewZoomMinimum = 0.5F;
constexpr float kViewZoomMaximum = 6.0F;
constexpr float kViewOffsetMinimum = -2.0F;
constexpr float kViewOffsetMaximum = 2.0F;

/**
 * @brief 人脸消费线程和 Godot 所有者线程之间的 latest-only 快照交接对象。
 *
 * 映射计算在发布前已经完成。本类只保存最新映射、取景状态和版本号，并在内部封装
 * 互斥锁；单一消费者按版本非阻塞读取。读取不会清空最近状态。
 */
class LatestTrackingChannel final {
public:
    LatestTrackingChannel() = default;
    ~LatestTrackingChannel() = default;

    LatestTrackingChannel(const LatestTrackingChannel&) = delete;
    LatestTrackingChannel& operator=(const LatestTrackingChannel&) = delete;
    LatestTrackingChannel(LatestTrackingChannel&&) = delete;
    LatestTrackingChannel& operator=(LatestTrackingChannel&&) = delete;

    void publish_mapping(Live2DMappingResult mapping) {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_mapping_ = std::move(mapping);
        ++snapshot_generation_;
    }

    std::optional<Live2DTrackingSnapshot> read_latest() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!latest_mapping_.has_value()) {
            return std::nullopt;
        }
        return Live2DTrackingSnapshot(
            snapshot_generation_, latest_mapping_.value(), view_);
    }

    void publish_view(const Live2DTrackingViewState& view) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        view_ = view;
        ++snapshot_generation_;
    }

    Live2DTrackingStatus read_status(
        std::size_t calibration_required) const {
        std::lock_guard<std::mutex> lock(mutex_);
        Live2DTrackingStatus status;
        status.calibration_required = calibration_required;
        status.view = view_;
        if (latest_mapping_.has_value()) {
            status.calibrated = latest_mapping_->calibrated();
            status.tracking_valid = latest_mapping_->tracking_valid();
            status.calibration_progress =
                latest_mapping_->calibration_progress();
            status.confidence = latest_mapping_->confidence();
        }
        return status;
    }

private:
    mutable std::mutex mutex_;
    std::optional<Live2DMappingResult> latest_mapping_;
    std::uint64_t snapshot_generation_ = 0;
    Live2DTrackingViewState view_;
};

}  // namespace

class Live2DTrackingSession::Impl final {
public:
    Impl(
        std::unique_ptr<VideoSource> video_input,
        std::unique_ptr<FacePipeline> face_pipeline,
        Live2DMappingConfig mapping_config,
        ResultObserver result_observer)
        : mapper_(mapping_config),
          calibration_required_(mapping_config.calibration_frames),
          runtime_(std::make_unique<VideoProcessingRuntime>(
              std::move(video_input),
              std::make_unique<FaceFrameConsumer>(
                  std::move(face_pipeline),
                  [this, observer = std::move(result_observer)](
                      const std::optional<FaceTrackingResult>& result) {
                      process_result(result);
                      if (observer) {
                          observer(result);
                      }
                  }))) {}

    ~Impl() {
        shutdown_noexcept();
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void start() {
        if (started_) {
            return;
        }
        runtime_->start();
        started_ = true;
    }

    void request_stop() noexcept {
        runtime_->request_stop();
    }

    void stop() {
        runtime_->stop();
        runtime_->rethrow_if_failed();
        started_ = false;
    }

    void rethrow_if_failed() const {
        runtime_->rethrow_if_failed();
    }

    Live2DTrackingStatus status() const {
        Live2DTrackingStatus result =
            channel_.read_status(calibration_required_);
        if (reset_pending_.load(std::memory_order_acquire)) {
            result.calibrated = false;
            result.calibration_progress = 0;
        }
        return result;
    }

    void request_calibration_reset() noexcept {
        reset_pending_.store(true, std::memory_order_release);
        reset_generation_.fetch_add(1, std::memory_order_release);
    }

    bool set_view(const Live2DTrackingViewState& view) noexcept {
        if (!std::isfinite(view.zoom()) || !std::isfinite(view.offset_x()) ||
            !std::isfinite(view.offset_y()) ||
            view.zoom() < kViewZoomMinimum || view.zoom() > kViewZoomMaximum ||
            view.offset_x() < kViewOffsetMinimum ||
            view.offset_x() > kViewOffsetMaximum ||
            view.offset_y() < kViewOffsetMinimum ||
            view.offset_y() > kViewOffsetMaximum) {
            return false;
        }
        channel_.publish_view(view);
        return true;
    }

    std::optional<Live2DTrackingSnapshot> read_snapshot_if_changed() {
        runtime_->rethrow_if_failed();
        std::optional<Live2DTrackingSnapshot> snapshot = channel_.read_latest();
        if (!snapshot.has_value() ||
            snapshot->snapshot_generation() == delivered_snapshot_generation_) {
            return std::nullopt;
        }
        delivered_snapshot_generation_ = snapshot->snapshot_generation();
        return snapshot;
    }

private:
    void process_result(const std::optional<FaceTrackingResult>& result) {
        const std::uint64_t requested_reset =
            reset_generation_.load(std::memory_order_acquire);
        if (requested_reset != applied_reset_generation_) {
            mapper_.reset_calibration();
            applied_reset_generation_ = requested_reset;
            reset_pending_.store(false, std::memory_order_release);
        }
        const FaceTrackingResult* face = nullptr;
        if (result.has_value()) {
            face = &result.value();
        }
        Live2DMappingResult mapping =
            mapper_.update(face, MonotonicClock::now_ns());
        channel_.publish_mapping(std::move(mapping));
    }

    void shutdown_noexcept() noexcept {
        try {
            request_stop();
            runtime_->stop();
        } catch (...) {
        }
        started_ = false;
    }

    Live2DFaceMapper mapper_;
    std::size_t calibration_required_ = 0;
    LatestTrackingChannel channel_;
    std::unique_ptr<VideoProcessingRuntime> runtime_;
    std::atomic<std::uint64_t> reset_generation_{0};
    std::atomic<bool> reset_pending_{false};
    std::uint64_t applied_reset_generation_ = 0;
    std::uint64_t delivered_snapshot_generation_ = 0;
    bool started_ = false;
};

Live2DTrackingSession::Live2DTrackingSession(
    std::unique_ptr<VideoSource> video_input,
    std::unique_ptr<FacePipeline> face_pipeline,
    Live2DMappingConfig mapping_config,
    ResultObserver result_observer)
    : impl_(std::make_unique<Impl>(
          std::move(video_input),
          std::move(face_pipeline),
          mapping_config,
          std::move(result_observer))) {}

Live2DTrackingSession::~Live2DTrackingSession() = default;

void Live2DTrackingSession::start() {
    impl_->start();
}

void Live2DTrackingSession::request_stop() noexcept {
    impl_->request_stop();
}

void Live2DTrackingSession::stop() {
    impl_->stop();
}

void Live2DTrackingSession::rethrow_if_failed() const {
    impl_->rethrow_if_failed();
}

std::optional<Live2DTrackingSnapshot>
Live2DTrackingSession::read_snapshot_if_changed() {
    return impl_->read_snapshot_if_changed();
}

Live2DTrackingStatus Live2DTrackingSession::status() const {
    return impl_->status();
}

void Live2DTrackingSession::request_calibration_reset() noexcept {
    impl_->request_calibration_reset();
}

bool Live2DTrackingSession::set_view(const Live2DTrackingViewState& view) noexcept {
    return impl_->set_view(view);
}

}  // namespace vtb
