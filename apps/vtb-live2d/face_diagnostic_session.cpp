#define MODULE_TAG "vtb_face_diagnostic"

#include "face_diagnostic_session.hpp"

#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/mpp_rc_defs.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_mpi_cmd.h>
#include <rockchip/rk_type.h>
#include <rockchip/rk_venc_cfg.h>

#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "vtb/time/monotonic_clock.hpp"

namespace vtb {
namespace {

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

std::uint32_t align_up(std::uint32_t value, std::uint32_t alignment) {
    const std::uint32_t remainder = value % alignment;
    if (remainder == 0) {
        return value;
    }
    return value + alignment - remainder;
}

std::chrono::nanoseconds frame_period(std::uint32_t fps) {
    if (fps == 0) {
        throw std::invalid_argument(
            "Face diagnostic maximum FPS must be positive");
    }
    return std::chrono::nanoseconds(1000000000ULL / fps);
}

class UniqueFd final {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : fd_(fd) {}
    ~UniqueFd() { reset(); }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}

    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    int get() const noexcept { return fd_; }

    int release() noexcept {
        const int result = fd_;
        fd_ = -1;
        return result;
    }

    void reset(int replacement = -1) noexcept {
        if (fd_ >= 0) {
            close(fd_);
        }
        fd_ = replacement;
    }

private:
    int fd_ = -1;
};

class MappedDmaBuffer final {
public:
    MappedDmaBuffer(const std::string& heap_path, std::size_t size)
        : size_(size) {
        UniqueFd heap(open(heap_path.c_str(), O_RDONLY | O_CLOEXEC));
        require(
            heap.get() >= 0,
            "open DMA heap failed: " + heap_path + " error=" +
                std::strerror(errno));

        dma_heap_allocation_data allocation{};
        allocation.len = size_;
        allocation.fd_flags = O_RDWR | O_CLOEXEC;
        const int allocation_result = ioctl(
            heap.get(), DMA_HEAP_IOCTL_ALLOC, &allocation);
        require(
            allocation_result == 0,
            "DMA_HEAP_IOCTL_ALLOC failed: " +
                std::string(std::strerror(errno)));
        fd_.reset(static_cast<int>(allocation.fd));

        mapping_ = mmap(
            nullptr,
            size_,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            fd_.get(),
            0);
        require(
            mapping_ != MAP_FAILED,
            "mmap DMA buffer failed: " +
                std::string(std::strerror(errno)));
    }

    ~MappedDmaBuffer() {
        if (mapping_ != MAP_FAILED) {
            munmap(mapping_, size_);
        }
    }

    MappedDmaBuffer(const MappedDmaBuffer&) = delete;
    MappedDmaBuffer& operator=(const MappedDmaBuffer&) = delete;
    MappedDmaBuffer(MappedDmaBuffer&&) = delete;
    MappedDmaBuffer& operator=(MappedDmaBuffer&&) = delete;

    int fd() const noexcept { return fd_.get(); }
    std::size_t size() const noexcept { return size_; }

    std::uint8_t* data() noexcept {
        return static_cast<std::uint8_t*>(mapping_);
    }

    void begin_cpu_write() {
        sync(DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE, "begin CPU write");
    }

    void end_cpu_write() {
        sync(DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE, "end CPU write");
    }

private:
    void sync(std::uint64_t flags, const std::string& operation) {
        dma_buf_sync request{};
        request.flags = flags;
        const int result = ioctl(fd_.get(), DMA_BUF_IOCTL_SYNC, &request);
        require(
            result == 0,
            operation + " failed: " + std::string(std::strerror(errno)));
    }

    UniqueFd fd_;
    void* mapping_ = MAP_FAILED;
    std::size_t size_ = 0;
};

class CpuWriteScope final {
public:
    explicit CpuWriteScope(MappedDmaBuffer& buffer) : buffer_(buffer) {
        buffer_.begin_cpu_write();
    }

    ~CpuWriteScope() {
        if (active_) {
            try {
                buffer_.end_cpu_write();
            } catch (...) {
            }
        }
    }

    CpuWriteScope(const CpuWriteScope&) = delete;
    CpuWriteScope& operator=(const CpuWriteScope&) = delete;
    CpuWriteScope(CpuWriteScope&&) = delete;
    CpuWriteScope& operator=(CpuWriteScope&&) = delete;

    void finish() {
        buffer_.end_cpu_write();
        active_ = false;
    }

private:
    MappedDmaBuffer& buffer_;
    bool active_ = true;
};

class MppPacketLease final {
public:
    MppPacketLease() = default;
    explicit MppPacketLease(MppPacket packet) : packet_(packet) {}
    ~MppPacketLease() { reset(); }

    MppPacketLease(const MppPacketLease&) = delete;
    MppPacketLease& operator=(const MppPacketLease&) = delete;

    MppPacketLease(MppPacketLease&& other) noexcept
        : packet_(other.packet_) {
        other.packet_ = nullptr;
    }

    MppPacketLease& operator=(MppPacketLease&& other) noexcept {
        if (this != &other) {
            reset();
            packet_ = other.packet_;
            other.packet_ = nullptr;
        }
        return *this;
    }

    const std::uint8_t* data() const noexcept {
        return static_cast<const std::uint8_t*>(
            mpp_packet_get_pos(packet_));
    }

    std::size_t size() const noexcept {
        return mpp_packet_get_length(packet_);
    }

    MppPacket get() const noexcept { return packet_; }

private:
    void reset() noexcept {
        if (packet_ != nullptr) {
            mpp_packet_deinit(&packet_);
        }
    }

    MppPacket packet_ = nullptr;
};

class MppEncoder final {
public:
    MppEncoder() = default;
    ~MppEncoder() { shutdown(); }

    MppEncoder(const MppEncoder&) = delete;
    MppEncoder& operator=(const MppEncoder&) = delete;
    MppEncoder(MppEncoder&&) = delete;
    MppEncoder& operator=(MppEncoder&&) = delete;

    void initialize(
        const MappedDmaBuffer& input,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t horizontal_stride,
        std::uint32_t vertical_stride,
        const FaceDiagnosticOutputConfiguration& config) {
        MPP_RET result = mpp_create(&context_, &api_);
        require(
            result == MPP_OK,
            "mpp_create failed: " + std::to_string(result));

        MppPollType blocking = MPP_POLL_BLOCK;
        result = api_->control(context_, MPP_SET_OUTPUT_TIMEOUT, &blocking);
        require(
            result == MPP_OK,
            "MPP_SET_OUTPUT_TIMEOUT failed: " + std::to_string(result));
        result = mpp_init(context_, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);
        require(
            result == MPP_OK,
            "mpp_init AVC failed: " + std::to_string(result));

        result = mpp_enc_cfg_init(&config_);
        require(
            result == MPP_OK,
            "mpp_enc_cfg_init failed: " + std::to_string(result));
        result = api_->control(context_, MPP_ENC_GET_CFG, config_);
        require(
            result == MPP_OK,
            "MPP_ENC_GET_CFG failed: " + std::to_string(result));

        set_config("prep:width", static_cast<RK_S32>(width));
        set_config("prep:height", static_cast<RK_S32>(height));
        set_config("prep:hor_stride", static_cast<RK_S32>(horizontal_stride));
        set_config("prep:ver_stride", static_cast<RK_S32>(vertical_stride));
        set_config("prep:format", MPP_FMT_YUV420SP);
        set_config("rc:mode", MPP_ENC_RC_MODE_CBR);
        set_config("rc:fps_in_flex", 0);
        set_config(
            "rc:fps_in_num", static_cast<RK_S32>(config.maximum_fps));
        set_config("rc:fps_in_denorm", 1);
        set_config("rc:fps_out_flex", 0);
        set_config(
            "rc:fps_out_num", static_cast<RK_S32>(config.maximum_fps));
        set_config("rc:fps_out_denorm", 1);
        set_config("rc:gop", static_cast<RK_S32>(config.gop));
        set_config("rc:bps_target", static_cast<RK_S32>(config.bitrate));
        set_config(
            "rc:bps_max", static_cast<RK_S32>(config.bitrate * 5U / 4U));
        set_config(
            "rc:bps_min", static_cast<RK_S32>(config.bitrate * 3U / 4U));
        set_config("codec:type", MPP_VIDEO_CodingAVC);
        set_config("h264:profile", 66);
        set_config("h264:level", 31);
        set_config("h264:cabac_en", 0);
        set_config("h264:trans8x8", 0);

        result = api_->control(context_, MPP_ENC_SET_CFG, config_);
        require(
            result == MPP_OK,
            "MPP_ENC_SET_CFG failed: " + std::to_string(result));
        MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
        result = api_->control(
            context_, MPP_ENC_SET_HEADER_MODE, &header_mode);
        require(
            result == MPP_OK,
            "MPP_ENC_SET_HEADER_MODE failed: " +
                std::to_string(result));

        MppBufferInfo input_info{};
        input_info.type = MPP_BUFFER_TYPE_EXT_DMA;
        input_info.size = input.size();
        input_info.fd = input.fd();
        input_info.index = -1;
        result = mpp_buffer_import(&input_buffer_, &input_info);
        require(
            result == MPP_OK,
            "mpp_buffer_import failed: " + std::to_string(result));

        result = mpp_frame_init(&frame_);
        require(
            result == MPP_OK,
            "mpp_frame_init failed: " + std::to_string(result));
        mpp_frame_set_width(frame_, width);
        mpp_frame_set_height(frame_, height);
        mpp_frame_set_hor_stride(frame_, horizontal_stride);
        mpp_frame_set_ver_stride(frame_, vertical_stride);
        mpp_frame_set_fmt(frame_, MPP_FMT_YUV420SP);
        mpp_frame_set_buffer(frame_, input_buffer_);
    }

    std::vector<std::uint8_t> codec_configuration() {
        std::vector<std::uint8_t> storage(4096);
        MppPacket packet = nullptr;
        MPP_RET result = mpp_packet_init(
            &packet, storage.data(), storage.size());
        require(
            result == MPP_OK && packet != nullptr,
            "mpp_packet_init for AVC header failed: " +
                std::to_string(result));
        MppPacketLease packet_lease(packet);
        mpp_packet_set_length(packet, 0);
        result = api_->control(context_, MPP_ENC_GET_HDR_SYNC, packet);
        require(
            result == MPP_OK,
            "MPP_ENC_GET_HDR_SYNC failed: " + std::to_string(result));
        require(
            packet_lease.data() != nullptr && packet_lease.size() > 0,
            "MPP returned an empty AVC header");
        return std::vector<std::uint8_t>(
            packet_lease.data(),
            packet_lease.data() + packet_lease.size());
    }

    MppPacketLease encode(
        std::uint64_t pts_ns,
        std::uint64_t& packet_pts_ns,
        std::uint64_t& packet_dts_ns,
        bool& keyframe) {
        mpp_frame_set_pts(frame_, static_cast<RK_S64>(pts_ns));
        mpp_frame_set_eos(frame_, 0);
        MPP_RET result = api_->encode_put_frame(context_, frame_);
        require(
            result == MPP_OK,
            "encode_put_frame failed: " + std::to_string(result));

        MppPacket packet = nullptr;
        result = api_->encode_get_packet(context_, &packet);
        require(
            result == MPP_OK && packet != nullptr,
            "encode_get_packet failed: " + std::to_string(result));
        MppPacketLease packet_lease(packet);
        require(
            packet_lease.data() != nullptr && packet_lease.size() > 0,
            "MPP returned an empty encoded packet");

        keyframe = contains_idr(packet_lease.data(), packet_lease.size());
        packet_pts_ns = timestamp_or_fallback(
            mpp_packet_get_pts(packet_lease.get()), pts_ns);
        packet_dts_ns = timestamp_or_fallback(
            mpp_packet_get_dts(packet_lease.get()), pts_ns);
        return packet_lease;
    }

private:
    void shutdown() noexcept {
        if (frame_ != nullptr) {
            mpp_frame_deinit(&frame_);
        }
        if (input_buffer_ != nullptr) {
            mpp_buffer_put(input_buffer_);
            input_buffer_ = nullptr;
        }
        if (config_ != nullptr) {
            mpp_enc_cfg_deinit(config_);
            config_ = nullptr;
        }
        if (context_ != nullptr) {
            mpp_destroy(context_);
            context_ = nullptr;
        }
        api_ = nullptr;
    }

    void set_config(const char* name, RK_S32 value) {
        const MPP_RET result = mpp_enc_cfg_set_s32(config_, name, value);
        require(
            result == MPP_OK,
            "mpp_enc_cfg_set_s32 failed for " + std::string(name) +
                ": " + std::to_string(result));
    }

    static bool contains_idr(
        const std::uint8_t* bytes,
        std::size_t size) noexcept {
        if (bytes == nullptr) {
            return false;
        }
        for (std::size_t index = 0; index + 3 < size; ++index) {
            std::size_t nal_index = size;
            if (bytes[index] == 0 && bytes[index + 1] == 0 &&
                bytes[index + 2] == 1) {
                nal_index = index + 3;
            } else if (
                index + 4 < size && bytes[index] == 0 &&
                bytes[index + 1] == 0 && bytes[index + 2] == 0 &&
                bytes[index + 3] == 1) {
                nal_index = index + 4;
            }
            if (nal_index < size && (bytes[nal_index] & 0x1fU) == 5U) {
                return true;
            }
        }
        return false;
    }

    static std::uint64_t timestamp_or_fallback(
        RK_S64 timestamp,
        std::uint64_t fallback) noexcept {
        if (timestamp < 0) {
            return fallback;
        }
        return static_cast<std::uint64_t>(timestamp);
    }

    MppCtx context_ = nullptr;
    MppApi* api_ = nullptr;
    MppEncCfg config_ = nullptr;
    MppBuffer input_buffer_ = nullptr;
    MppFrame frame_ = nullptr;
};

int canvas_coordinate(float value, std::uint32_t limit) noexcept {
    if (!std::isfinite(value) || limit == 0) {
        return 0;
    }
    long rounded = std::lround(value);
    if (rounded < 0) {
        rounded = 0;
    }
    const long maximum = static_cast<long>(limit - 1U);
    if (rounded > maximum) {
        rounded = maximum;
    }
    return static_cast<int>(rounded);
}

}  // namespace

class FaceDiagnosticSession::Impl final {
public:
    Impl(
        FaceDiagnosticOutputConfiguration config,
        std::uint32_t frame_width,
        std::uint32_t frame_height,
        std::string dma_heap_path,
        EncodedVideoSink output_sink)
        : config_(std::move(config)),
          width_(frame_width),
          height_(frame_height),
          horizontal_stride_(align_up(frame_width, 16)),
          vertical_stride_(align_up(frame_height, 16)),
          dma_heap_path_(std::move(dma_heap_path)),
          output_sink_(output_sink),
          frame_period_(frame_period(config_.maximum_fps)) {
        if (!config_.enabled || width_ == 0 || height_ == 0 ||
            width_ % 2U != 0U || height_ % 2U != 0U ||
            dma_heap_path_.empty() || output_sink_.context == nullptr ||
            output_sink_.configure_h264 == nullptr ||
            output_sink_.publish == nullptr) {
            throw std::invalid_argument(
                "Invalid face diagnostic session configuration");
        }
    }

    ~Impl() { shutdown_noexcept(); }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void start() {
        if (started_) {
            throw std::logic_error(
                "Face diagnostic session can only be started once");
        }
        const std::size_t buffer_size =
            static_cast<std::size_t>(horizontal_stride_) * vertical_stride_ *
            3U / 2U;
        buffer_ = std::make_unique<MappedDmaBuffer>(
            dma_heap_path_, buffer_size);
        encoder_ = std::make_unique<MppEncoder>();
        encoder_->initialize(
            *buffer_,
            width_,
            height_,
            horizontal_stride_,
            vertical_stride_,
            config_);
        std::vector<std::uint8_t> codec_configuration =
            encoder_->codec_configuration();
        H264StreamFormat format;
        format.width = width_;
        format.height = height_;
        format.fps = config_.maximum_fps;
        format.codec_configuration = codec_configuration.data();
        format.codec_configuration_size = codec_configuration.size();
        if (output_sink_.configure_h264(output_sink_.context, &format) != 0) {
            throw std::runtime_error(
                "Media output rejected face diagnostic H.264 configuration");
        }
        started_ = true;
        worker_ = std::thread(&Impl::worker_loop, this);
    }

    void publish(const std::optional<FaceTrackingResult>& result) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ || stop_requested_) {
            return;
        }
        if (generation_ != consumed_generation_) {
            ++replaced_count_;
        }
        latest_result_ = result;
        ++generation_;
        ++published_count_;
        condition_.notify_one();
    }

    void request_stop() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_requested_ = true;
        }
        condition_.notify_all();
    }

    void stop() {
        request_stop();
        if (worker_.joinable()) {
            worker_.join();
        }
        started_ = false;
        std::cout << "VTB_FACE_DIAGNOSTIC_RESULT published="
                  << published_count_ << " encoded=" << encoded_count_
                  << " replaced=" << replaced_count_ << "\n";
        rethrow_if_failed();
    }

    void rethrow_if_failed() const {
        std::exception_ptr failure;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            failure = failure_;
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

private:
    void shutdown_noexcept() noexcept {
        try {
            request_stop();
            if (worker_.joinable()) {
                worker_.join();
            }
        } catch (...) {
        }
        started_ = false;
    }

    void worker_loop() noexcept {
        try {
            auto next_frame_time = std::chrono::steady_clock::now();
            while (true) {
                std::optional<FaceTrackingResult> result;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    condition_.wait(lock, [this]() {
                        return stop_requested_ ||
                            generation_ != consumed_generation_;
                    });
                    if (stop_requested_) {
                        return;
                    }
                    const auto now = std::chrono::steady_clock::now();
                    if (now < next_frame_time) {
                        condition_.wait_until(
                            lock,
                            next_frame_time,
                            [this]() { return stop_requested_; });
                        if (stop_requested_) {
                            return;
                        }
                    }
                    result = latest_result_;
                    consumed_generation_ = generation_;
                }
                render(result);
                encode();
                ++encoded_count_;
                next_frame_time =
                    std::chrono::steady_clock::now() + frame_period_;
            }
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!failure_) {
                    failure_ = std::current_exception();
                }
                stop_requested_ = true;
            }
            condition_.notify_all();
        }
    }

    void render(const std::optional<FaceTrackingResult>& result) {
        CpuWriteScope write_scope(*buffer_);
        std::uint8_t* const data = buffer_->data();
        const std::size_t y_size =
            static_cast<std::size_t>(horizontal_stride_) * vertical_stride_;
        std::fill(data, data + y_size, static_cast<std::uint8_t>(16));
        std::fill(
            data + y_size,
            data + buffer_->size(),
            static_cast<std::uint8_t>(128));
        if (result.has_value()) {
            draw_result(result.value());
        }
        write_scope.finish();
    }

    void draw_result(const FaceTrackingResult& result) noexcept {
        const FaceBox& box = result.box();
        const int left = canvas_coordinate(box.x(), width_);
        const int top = canvas_coordinate(box.y(), height_);
        const int right = canvas_coordinate(box.x() + box.width(), width_);
        const int bottom = canvas_coordinate(box.y() + box.height(), height_);
        draw_rectangle(left, top, right, bottom, 176);
        for (const FaceLandmark& landmark : result.landmarks()) {
            const int x = canvas_coordinate(landmark.x(), width_);
            const int y = canvas_coordinate(landmark.y(), height_);
            draw_square(x, y, 2, 235);
        }
        std::uint32_t available_width = 0;
        if (width_ > 20U) {
            available_width = width_ - 20U;
        }
        const float confidence_width =
            static_cast<float>(available_width) * result.confidence();
        const int bar_right = 10 + canvas_coordinate(
            confidence_width, available_width + 1U);
        draw_rectangle(10, 8, bar_right, 13, 200);
    }

    void draw_rectangle(
        int left,
        int top,
        int right,
        int bottom,
        std::uint8_t value) noexcept {
        if (right < left) {
            std::swap(left, right);
        }
        if (bottom < top) {
            std::swap(top, bottom);
        }
        for (int x = left; x <= right; ++x) {
            set_luma(x, top, value);
            set_luma(x, bottom, value);
        }
        for (int y = top; y <= bottom; ++y) {
            set_luma(left, y, value);
            set_luma(right, y, value);
        }
    }

    void draw_square(
        int center_x,
        int center_y,
        int radius,
        std::uint8_t value) noexcept {
        for (int y = center_y - radius; y <= center_y + radius; ++y) {
            for (int x = center_x - radius; x <= center_x + radius; ++x) {
                set_luma(x, y, value);
            }
        }
    }

    void set_luma(int x, int y, std::uint8_t value) noexcept {
        if (x < 0 || y < 0 || x >= static_cast<int>(width_) ||
            y >= static_cast<int>(height_)) {
            return;
        }
        const std::size_t offset =
            static_cast<std::size_t>(y) * horizontal_stride_ +
            static_cast<std::size_t>(x);
        buffer_->data()[offset] = value;
    }

    void encode() {
        const std::uint64_t pts_ns = MonotonicClock::now_ns();
        std::uint64_t packet_pts_ns = pts_ns;
        std::uint64_t packet_dts_ns = pts_ns;
        bool keyframe = false;
        MppPacketLease packet = encoder_->encode(
            pts_ns, packet_pts_ns, packet_dts_ns, keyframe);

        EncodedVideoPacketView view;
        view.data = packet.data();
        view.size = packet.size();
        view.pts_ns = packet_pts_ns;
        view.dts_ns = packet_dts_ns;
        view.duration_ns =
            static_cast<std::uint64_t>(frame_period_.count());
        view.sequence = sequence_;
        view.keyframe = 0;
        if (keyframe) {
            view.keyframe = 1;
        }
        ++sequence_;
        if (output_sink_.publish(output_sink_.context, &view) != 0) {
            throw std::runtime_error(
                "Media output rejected face diagnostic packet");
        }
    }

    FaceDiagnosticOutputConfiguration config_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t horizontal_stride_ = 0;
    std::uint32_t vertical_stride_ = 0;
    std::string dma_heap_path_;
    EncodedVideoSink output_sink_;
    std::chrono::nanoseconds frame_period_;
    std::unique_ptr<MappedDmaBuffer> buffer_;
    std::unique_ptr<MppEncoder> encoder_;
    std::thread worker_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<FaceTrackingResult> latest_result_;
    std::exception_ptr failure_;
    std::uint64_t generation_ = 0;
    std::uint64_t consumed_generation_ = 0;
    std::uint64_t published_count_ = 0;
    std::uint64_t replaced_count_ = 0;
    std::uint64_t encoded_count_ = 0;
    std::uint64_t sequence_ = 0;
    bool started_ = false;
    bool stop_requested_ = false;
};

FaceDiagnosticSession::FaceDiagnosticSession(
    FaceDiagnosticOutputConfiguration config,
    std::uint32_t frame_width,
    std::uint32_t frame_height,
    std::string dma_heap_path,
    EncodedVideoSink output_sink)
    : impl_(std::make_unique<Impl>(
          std::move(config),
          frame_width,
          frame_height,
          std::move(dma_heap_path),
          output_sink)) {}

FaceDiagnosticSession::~FaceDiagnosticSession() = default;

void FaceDiagnosticSession::start() { impl_->start(); }

void FaceDiagnosticSession::publish(
    const std::optional<FaceTrackingResult>& result) {
    impl_->publish(result);
}

void FaceDiagnosticSession::request_stop() noexcept {
    impl_->request_stop();
}

void FaceDiagnosticSession::stop() { impl_->stop(); }

void FaceDiagnosticSession::rethrow_if_failed() const {
    impl_->rethrow_if_failed();
}

}  // namespace vtb
