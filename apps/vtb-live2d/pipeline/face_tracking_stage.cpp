#include "pipeline/face_tracking_stage.hpp"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace vtb {
namespace {

constexpr float kViewZoomMinimum = 0.5F;
constexpr float kViewZoomMaximum = 6.0F;
constexpr float kViewOffsetMinimum = -2.0F;
constexpr float kViewOffsetMaximum = 2.0F;

const Live2DTrackingPipeline& require_tracking_pipeline(
    const std::unique_ptr<Live2DTrackingPipeline>& tracking_pipeline) {
    if (!tracking_pipeline) {
        throw std::invalid_argument(
            "FaceTrackingStage tracking pipeline must not be null");
    }
    return *tracking_pipeline;
}

}  // namespace

FaceTrackingStage::FaceTrackingStage(
    std::unique_ptr<Live2DTrackingPipeline> tracking_pipeline,
    VideoFrameChannel& input_channel,
    Live2DTrackingChannel& output_channel)
    : tracking_pipeline_(std::move(tracking_pipeline)),
      calibration_required_(
          require_tracking_pipeline(tracking_pipeline_).calibration_required()),
      input_channel_(input_channel),
      output_channel_(output_channel) {}

FaceTrackingStage::~FaceTrackingStage() {
    shutdown_noexcept();
}

void FaceTrackingStage::start() {
    if (started_) {
        throw std::logic_error("FaceTrackingStage can only be started once");
    }

    worker_ = std::thread(&FaceTrackingStage::thread_main, this);
    std::unique_lock<std::mutex> lock(lifecycle_mutex_);
    lifecycle_condition_.wait(lock, [this]() {
        return initialization_finished_;
    });
    const std::exception_ptr initialization_failure = failure_;
    lock.unlock();

    if (initialization_failure) {
        if (worker_.joinable()) {
            worker_.join();
        }
        std::rethrow_exception(initialization_failure);
    }
    started_ = true;
}

void FaceTrackingStage::request_stop() noexcept {
    input_channel_.close();
}

void FaceTrackingStage::stop() {
    request_stop();
    if (worker_.joinable()) {
        worker_.join();
    }
    started_ = false;
}

void FaceTrackingStage::rethrow_if_failed() const {
    std::exception_ptr failure;
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        failure = failure_;
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
}

Live2DTrackingStatus FaceTrackingStage::status() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    Live2DTrackingStatus result;
    result.calibration_required = calibration_required_;
    result.view = view_;
    if (latest_mapping_.has_value()) {
        result.calibrated = latest_mapping_->calibrated();
        result.tracking_valid = latest_mapping_->tracking_valid();
        result.calibration_progress =
            latest_mapping_->calibration_progress();
        result.confidence = latest_mapping_->confidence();
    }
    if (reset_pending_.load()) {
        result.calibrated = false;
        result.calibration_progress = 0;
    }
    return result;
}

void FaceTrackingStage::request_calibration_reset() noexcept {
    reset_pending_.store(true);
    reset_generation_.fetch_add(1);
}

bool FaceTrackingStage::set_view(
    const Live2DTrackingViewState& view) noexcept {
    if (!std::isfinite(view.zoom()) || !std::isfinite(view.offset_x()) ||
        !std::isfinite(view.offset_y()) ||
        view.zoom() < kViewZoomMinimum || view.zoom() > kViewZoomMaximum ||
        view.offset_x() < kViewOffsetMinimum ||
        view.offset_x() > kViewOffsetMaximum ||
        view.offset_y() < kViewOffsetMinimum ||
        view.offset_y() > kViewOffsetMaximum) {
        return false;
    }

    try {
        std::lock_guard<std::mutex> lock(state_mutex_);
        view_ = view;
        ++snapshot_generation_;
        if (latest_mapping_.has_value()) {
            const messaging::ChannelPublishResult result =
                publish_snapshot_locked();
            if (result == messaging::ChannelPublishResult::closed ||
                result == messaging::ChannelPublishResult::finished) {
                return false;
            }
        }
    } catch (...) {
        record_failure(std::current_exception());
        input_channel_.close();
        output_channel_.close();
        return false;
    }
    return true;
}

std::uint64_t FaceTrackingStage::processed_frame_count() const noexcept {
    return processed_frame_count_.load();
}

void FaceTrackingStage::thread_main() noexcept {
    bool source_began = false;
    bool completed = false;
    try {
        tracking_pipeline_->begin_source(generation_);
        source_began = true;
        mark_initialization_finished();

        while (true) {
            std::optional<DecodedFrameLease> frame =
                input_channel_.wait_pop();
            if (!frame.has_value()) {
                break;
            }
            process_frame(std::move(frame.value()));
            processed_frame_count_.fetch_add(1);
        }
        completed = true;
    } catch (...) {
        record_failure(std::current_exception());
        input_channel_.close();
    }

    if (source_began) {
        tracking_pipeline_->retire_source(generation_);
    }
    if (completed) {
        output_channel_.finish();
    } else {
        output_channel_.close();
    }
    mark_initialization_finished();
}

void FaceTrackingStage::process_frame(DecodedFrameLease frame) {
    const std::uint64_t requested_reset =
        reset_generation_.load();
    if (requested_reset != applied_reset_generation_) {
        tracking_pipeline_->reset_calibration();
        applied_reset_generation_ = requested_reset;
        reset_pending_.store(false);
    }

    Live2DMappingResult mapping =
        tracking_pipeline_->process(frame.view());

    std::lock_guard<std::mutex> lock(state_mutex_);
    latest_mapping_ = std::move(mapping);
    ++snapshot_generation_;
    const messaging::ChannelPublishResult result =
        publish_snapshot_locked();
    if (result == messaging::ChannelPublishResult::closed ||
        result == messaging::ChannelPublishResult::finished) {
        input_channel_.close();
        return;
    }
}

messaging::ChannelPublishResult
FaceTrackingStage::publish_snapshot_locked() {
    return output_channel_.publish(Live2DTrackingSnapshot(
        snapshot_generation_, latest_mapping_.value(), view_));
}

void FaceTrackingStage::mark_initialization_finished() noexcept {
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        initialization_finished_ = true;
    }
    lifecycle_condition_.notify_all();
}

void FaceTrackingStage::record_failure(
    std::exception_ptr failure) noexcept {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!failure_) {
        failure_ = std::move(failure);
    }
}

void FaceTrackingStage::shutdown_noexcept() noexcept {
    try {
        stop();
    } catch (...) {
    }
}

}  // namespace vtb
