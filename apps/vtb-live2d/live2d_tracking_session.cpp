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

class Live2DTrackingState final {
public:
    explicit Live2DTrackingState(Live2DMappingConfig config)
        : mapper_(config), calibration_required_(config.calibration_frames) {}
    ~Live2DTrackingState() = default;

    Live2DTrackingState(const Live2DTrackingState&) = delete;
    Live2DTrackingState& operator=(const Live2DTrackingState&) = delete;
    Live2DTrackingState(Live2DTrackingState&&) = delete;
    Live2DTrackingState& operator=(Live2DTrackingState&&) = delete;

    void publish(const std::optional<FaceTrackingResult>& result) {
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
        Live2DMappingResult mapped =
            mapper_.update(face, MonotonicClock::now_ns());
        std::lock_guard<std::mutex> lock(mutex_);
        latest_ = std::move(mapped);
        ++snapshot_generation_;
    }

    std::optional<Live2DTrackingSnapshot> read() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!latest_.has_value()) {
            return std::nullopt;
        }
        return Live2DTrackingSnapshot(snapshot_generation_, latest_.value(), view_);
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
        std::lock_guard<std::mutex> lock(mutex_);
        view_ = view;
        ++snapshot_generation_;
        return true;
    }

    Live2DTrackingStatus status() const {
        std::lock_guard<std::mutex> lock(mutex_);
        Live2DTrackingStatus status;
        status.calibration_required = calibration_required_;
        status.view = view_;
        if (latest_.has_value()) {
            status.calibrated = latest_->calibrated();
            status.tracking_valid = latest_->tracking_valid();
            status.calibration_progress = latest_->calibration_progress();
            status.confidence = latest_->confidence();
        }
        if (reset_pending_.load(std::memory_order_acquire)) {
            status.calibrated = false;
            status.calibration_progress = 0;
        }
        return status;
    }

private:
    mutable std::mutex mutex_;
    Live2DFaceMapper mapper_;
    std::size_t calibration_required_ = 0;
    std::optional<Live2DMappingResult> latest_;
    std::atomic<std::uint64_t> reset_generation_{0};
    std::atomic<bool> reset_pending_{false};
    std::uint64_t applied_reset_generation_ = 0;
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
        : state_(mapping_config),
          runtime_(std::make_unique<VideoProcessingRuntime>(
              std::move(video_input),
              std::make_unique<FaceFrameConsumer>(
                  std::move(face_pipeline),
                  [this, observer = std::move(result_observer)](
                      const std::optional<FaceTrackingResult>& result) {
                      state_.publish(result);
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

    Live2DTrackingStatus status() const { return state_.status(); }
    void request_calibration_reset() noexcept { state_.request_calibration_reset(); }
    bool set_view(const Live2DTrackingViewState& view) noexcept { return state_.set_view(view); }

    std::optional<Live2DTrackingSnapshot> poll_snapshot_update() {
        runtime_->rethrow_if_failed();
        std::optional<Live2DTrackingSnapshot> snapshot = state_.read();
        if (!snapshot.has_value() ||
            snapshot->snapshot_generation() == delivered_snapshot_generation_) {
            return std::nullopt;
        }
        delivered_snapshot_generation_ = snapshot->snapshot_generation();
        return snapshot;
    }

private:
    void shutdown_noexcept() noexcept {
        try {
            request_stop();
            runtime_->stop();
        } catch (...) {
        }
        started_ = false;
    }

    Live2DTrackingState state_;
    std::unique_ptr<VideoProcessingRuntime> runtime_;
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

std::optional<Live2DTrackingSnapshot> Live2DTrackingSession::poll_snapshot_update() {
    return impl_->poll_snapshot_update();
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
