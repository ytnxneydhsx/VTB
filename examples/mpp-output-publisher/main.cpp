#define MODULE_TAG "vtb_mpp_output_publisher"

#include <cstddef>

#include <rga/im2d.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/mpp_rc_defs.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_mpi_cmd.h>
#include <rockchip/rk_type.h>
#include <rockchip/rk_venc_cfg.h>
#include <rockchip/rk_venc_cmd.h>
#include <rockchip/rk_venc_rc.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "vtb/output/output_manager.hpp"
#include "vtb/output/rtsp_output.hpp"

namespace {

volatile std::sig_atomic_t stop_requested = 0;

void handle_signal(int) { stop_requested = 1; }

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

std::uint64_t steady_now_ns() {
    timespec value{};
    const int result = clock_gettime(CLOCK_MONOTONIC, &value);
    require(result == 0,
            "clock_gettime(CLOCK_MONOTONIC) failed: " +
                std::string(std::strerror(errno)));
    return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(value.tv_nsec);
}

double milliseconds(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

class PublisherOptions {
public:
    std::string rtsp_url;
    std::string stream_id = "face3d-video";
    std::string dma_heap = "/dev/dma_heap/system-uncached";
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    std::uint32_t frame_rate = 30;
    std::uint32_t bitrate = 1200000;
    std::uint32_t gop = 30;
    std::size_t queue_capacity = 8;
    bool timed_input = false;
    std::uint32_t diagnostics_port = 0;
    std::string diagnostics_page;
};

class UniqueFd {
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
        const int value = fd_;
        fd_ = -1;
        return value;
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

class MappedDmaBuffer {
public:
    MappedDmaBuffer(const std::string& heap_path, std::size_t size)
        : size_(size) {
        UniqueFd heap(open(heap_path.c_str(), O_RDONLY | O_CLOEXEC));
        require(heap.get() >= 0,
                "open DMA heap failed: " + heap_path + " error=" +
                    std::strerror(errno));

        dma_heap_allocation_data allocation{};
        allocation.len = size_;
        allocation.fd_flags = O_RDWR | O_CLOEXEC;
        const int allocation_result = ioctl(
            heap.get(), DMA_HEAP_IOCTL_ALLOC, &allocation);
        require(allocation_result == 0,
                "DMA_HEAP_IOCTL_ALLOC failed: " +
                    std::string(std::strerror(errno)));
        fd_.reset(static_cast<int>(allocation.fd));

        mapping_ = mmap(nullptr, size_, PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd_.get(), 0);
        require(mapping_ != MAP_FAILED,
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
        require(result == 0,
                operation + " failed: " + std::string(std::strerror(errno)));
    }

    UniqueFd fd_;
    void* mapping_ = MAP_FAILED;
    std::size_t size_ = 0;
};

class CpuWriteScope {
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

class RgaConverter {
public:
    RgaConverter() = default;

    ~RgaConverter() {
        if (destination_handle_ != 0) {
            releasebuffer_handle(destination_handle_);
        }
        if (source_handle_ != 0) {
            releasebuffer_handle(source_handle_);
        }
    }

    RgaConverter(const RgaConverter&) = delete;
    RgaConverter& operator=(const RgaConverter&) = delete;
    RgaConverter(RgaConverter&&) = delete;
    RgaConverter& operator=(RgaConverter&&) = delete;

    void initialize(const MappedDmaBuffer& source,
                    const MappedDmaBuffer& destination,
                    std::uint32_t width, std::uint32_t height,
                    std::uint32_t source_stride,
                    std::uint32_t destination_stride,
                    std::uint32_t destination_vertical_stride) {
        source_handle_ = importbuffer_fd(
            source.fd(), static_cast<int>(source.size()));
        require(source_handle_ != 0, "RGA source DMA-BUF import failed");
        destination_handle_ = importbuffer_fd(
            destination.fd(), static_cast<int>(destination.size()));
        require(destination_handle_ != 0,
                "RGA destination DMA-BUF import failed");

        source_buffer_ = wrapbuffer_handle_t(
            source_handle_, static_cast<int>(width), static_cast<int>(height),
            static_cast<int>(source_stride), static_cast<int>(height),
            RK_FORMAT_BGR_888);
        destination_buffer_ = wrapbuffer_handle_t(
            destination_handle_, static_cast<int>(width), static_cast<int>(height),
            static_cast<int>(destination_stride),
            static_cast<int>(destination_vertical_stride),
            RK_FORMAT_YCbCr_420_SP);

        im_rect source_rect{};
        im_rect destination_rect{};
        rga_buffer_t pattern_buffer{};
        im_rect pattern_rect{};
        const IM_STATUS check = imcheck_t(
            source_buffer_, destination_buffer_, pattern_buffer,
            source_rect, destination_rect, pattern_rect, 0);
        require(check == IM_STATUS_NOERROR || check == IM_STATUS_SUCCESS,
                "RGA imcheck failed: " +
                    std::string(imStrError_t(check)));
    }

    void convert() {
        const IM_STATUS status = imcvtcolor_t(
            source_buffer_, destination_buffer_, RK_FORMAT_BGR_888,
            RK_FORMAT_YCbCr_420_SP, IM_RGB_TO_YUV_BT601_LIMIT, 1);
        require(status == IM_STATUS_SUCCESS,
                "RGA BGR to NV12 failed: " +
                    std::string(imStrError_t(status)));
    }

private:
    rga_buffer_handle_t source_handle_ = 0;
    rga_buffer_handle_t destination_handle_ = 0;
    rga_buffer_t source_buffer_{};
    rga_buffer_t destination_buffer_{};
};

class UniqueMppPacket {
public:
    UniqueMppPacket() = default;

    ~UniqueMppPacket() {
        if (packet_ != nullptr) {
            mpp_packet_deinit(&packet_);
        }
    }

    UniqueMppPacket(const UniqueMppPacket&) = delete;
    UniqueMppPacket& operator=(const UniqueMppPacket&) = delete;
    UniqueMppPacket(UniqueMppPacket&&) = delete;
    UniqueMppPacket& operator=(UniqueMppPacket&&) = delete;

    MppPacket* output() noexcept { return &packet_; }
    MppPacket get() const noexcept { return packet_; }

    void initialize(void* data, std::size_t size) {
        const MPP_RET result = mpp_packet_init(&packet_, data, size);
        require(result == MPP_OK,
                "mpp_packet_init failed: " + std::to_string(result));
    }

private:
    MppPacket packet_ = nullptr;
};

class MppEncoder {
public:
    MppEncoder() = default;

    ~MppEncoder() {
        if (frame_ != nullptr) {
            mpp_frame_deinit(&frame_);
        }
        if (input_buffer_ != nullptr) {
            mpp_buffer_put(input_buffer_);
        }
        if (config_ != nullptr) {
            mpp_enc_cfg_deinit(config_);
        }
        if (context_ != nullptr) {
            mpp_destroy(context_);
        }
    }

    MppEncoder(const MppEncoder&) = delete;
    MppEncoder& operator=(const MppEncoder&) = delete;
    MppEncoder(MppEncoder&&) = delete;
    MppEncoder& operator=(MppEncoder&&) = delete;

    void initialize(const MappedDmaBuffer& input,
                    const PublisherOptions& options,
                    std::uint32_t horizontal_stride,
                    std::uint32_t vertical_stride) {
        MPP_RET result = mpp_create(&context_, &api_);
        require(result == MPP_OK,
                "mpp_create failed: " + std::to_string(result));

        MppPollType blocking = MPP_POLL_BLOCK;
        result = api_->control(context_, MPP_SET_OUTPUT_TIMEOUT, &blocking);
        require(result == MPP_OK,
                "MPP_SET_OUTPUT_TIMEOUT failed: " + std::to_string(result));
        result = mpp_init(context_, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);
        require(result == MPP_OK,
                "mpp_init AVC failed: " + std::to_string(result));

        result = mpp_enc_cfg_init(&config_);
        require(result == MPP_OK,
                "mpp_enc_cfg_init failed: " + std::to_string(result));
        result = api_->control(context_, MPP_ENC_GET_CFG, config_);
        require(result == MPP_OK,
                "MPP_ENC_GET_CFG failed: " + std::to_string(result));

        set_config("prep:width", static_cast<RK_S32>(options.width));
        set_config("prep:height", static_cast<RK_S32>(options.height));
        set_config("prep:hor_stride", static_cast<RK_S32>(horizontal_stride));
        set_config("prep:ver_stride", static_cast<RK_S32>(vertical_stride));
        set_config("prep:format", MPP_FMT_YUV420SP);
        set_config("rc:mode", MPP_ENC_RC_MODE_CBR);
        set_config("rc:fps_in_flex", 0);
        set_config("rc:fps_in_num", static_cast<RK_S32>(options.frame_rate));
        set_config("rc:fps_in_denorm", 1);
        set_config("rc:fps_out_flex", 0);
        set_config("rc:fps_out_num", static_cast<RK_S32>(options.frame_rate));
        set_config("rc:fps_out_denorm", 1);
        set_config("rc:gop", static_cast<RK_S32>(options.gop));
        set_config("rc:bps_target", static_cast<RK_S32>(options.bitrate));
        set_config("rc:bps_max", static_cast<RK_S32>(options.bitrate * 5 / 4));
        set_config("rc:bps_min", static_cast<RK_S32>(options.bitrate * 3 / 4));
        set_config("codec:type", MPP_VIDEO_CodingAVC);
        set_config("h264:profile", 66);
        set_config("h264:level", 31);
        set_config("h264:cabac_en", 0);
        set_config("h264:trans8x8", 0);

        result = api_->control(context_, MPP_ENC_SET_CFG, config_);
        require(result == MPP_OK,
                "MPP_ENC_SET_CFG failed: " + std::to_string(result));
        MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
        result = api_->control(context_, MPP_ENC_SET_HEADER_MODE, &header_mode);
        require(result == MPP_OK,
                "MPP_ENC_SET_HEADER_MODE failed: " + std::to_string(result));

        MppBufferInfo input_info{};
        input_info.type = MPP_BUFFER_TYPE_EXT_DMA;
        input_info.size = input.size();
        input_info.fd = input.fd();
        input_info.index = -1;
        result = mpp_buffer_import(&input_buffer_, &input_info);
        require(result == MPP_OK,
                "mpp_buffer_import failed: " + std::to_string(result));

        result = mpp_frame_init(&frame_);
        require(result == MPP_OK,
                "mpp_frame_init failed: " + std::to_string(result));
        mpp_frame_set_width(frame_, options.width);
        mpp_frame_set_height(frame_, options.height);
        mpp_frame_set_hor_stride(frame_, horizontal_stride);
        mpp_frame_set_ver_stride(frame_, vertical_stride);
        mpp_frame_set_fmt(frame_, MPP_FMT_YUV420SP);
        mpp_frame_set_buffer(frame_, input_buffer_);
    }

    std::vector<std::uint8_t> codec_configuration() {
        std::vector<std::uint8_t> storage(4096);
        UniqueMppPacket packet;
        packet.initialize(storage.data(), storage.size());
        mpp_packet_set_length(packet.get(), 0);
        const MPP_RET result = api_->control(
            context_, MPP_ENC_GET_HDR_SYNC, packet.get());
        require(result == MPP_OK,
                "MPP_ENC_GET_HDR_SYNC failed: " + std::to_string(result));
        require(packet.get() != nullptr, "MPP returned no AVC header packet");
        return copy_packet(packet.get(), "AVC header");
    }

    std::shared_ptr<const std::vector<std::uint8_t>> encode(
        std::uint64_t pts_ns, bool& keyframe,
        std::uint64_t& packet_pts_ns, std::uint64_t& packet_dts_ns) {
        mpp_frame_set_pts(frame_, static_cast<RK_S64>(pts_ns));
        mpp_frame_set_eos(frame_, 0);
        MPP_RET result = api_->encode_put_frame(context_, frame_);
        require(result == MPP_OK,
                "encode_put_frame failed: " + std::to_string(result));

        UniqueMppPacket packet;
        result = api_->encode_get_packet(context_, packet.output());
        require(result == MPP_OK,
                "encode_get_packet failed: " + std::to_string(result));
        require(packet.get() != nullptr, "MPP returned no encoded packet");

        auto payload = std::make_shared<const std::vector<std::uint8_t>>(
            copy_packet(packet.get(), "encoded frame"));
        keyframe = contains_idr(*payload);
        packet_pts_ns = timestamp_or_fallback(
            mpp_packet_get_pts(packet.get()), pts_ns);
        packet_dts_ns = timestamp_or_fallback(
            mpp_packet_get_dts(packet.get()), pts_ns);
        return payload;
    }

private:
    void set_config(const char* name, RK_S32 value) {
        const MPP_RET result = mpp_enc_cfg_set_s32(config_, name, value);
        require(result == MPP_OK,
                "mpp_enc_cfg_set_s32 failed for " + std::string(name) +
                    ": " + std::to_string(result));
    }

    static std::vector<std::uint8_t> copy_packet(
        MppPacket packet, const std::string& description) {
        void* position = mpp_packet_get_pos(packet);
        const std::size_t length = mpp_packet_get_length(packet);
        require(position != nullptr, description + " has no data");
        require(length > 0, description + " is empty");
        const auto* first = static_cast<const std::uint8_t*>(position);
        return std::vector<std::uint8_t>(first, first + length);
    }

    static bool contains_idr(const std::vector<std::uint8_t>& bytes) {
        for (std::size_t index = 0; index + 4 < bytes.size(); ++index) {
            std::size_t nal_index = 0;
            if (bytes[index] == 0 && bytes[index + 1] == 0 &&
                bytes[index + 2] == 1) {
                nal_index = index + 3;
            } else if (index + 5 < bytes.size() && bytes[index] == 0 &&
                       bytes[index + 1] == 0 && bytes[index + 2] == 0 &&
                       bytes[index + 3] == 1) {
                nal_index = index + 4;
            }
            if (nal_index != 0 && (bytes[nal_index] & 0x1fU) == 5U) {
                return true;
            }
        }
        return false;
    }

    static std::uint64_t timestamp_or_fallback(
        RK_S64 timestamp, std::uint64_t fallback) {
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

class FrameTiming {
public:
    std::uint64_t session_id = 0;
    std::uint64_t frame_id = 0;
    std::uint64_t capture_sequence = 0;
    std::uint64_t capture_ready_ns = 0;
    std::uint64_t inference_start_ns = 0;
    std::uint64_t detector_start_ns = 0;
    std::uint64_t detector_end_ns = 0;
    std::uint64_t landmark_start_ns = 0;
    std::uint64_t landmark_end_ns = 0;
    std::uint64_t points_ready_ns = 0;
    std::uint64_t canvas_ready_ns = 0;
};

class FrameTrace {
public:
    FrameTiming python;
    std::uint64_t cpp_frame_received_ns = 0;
    std::uint64_t rga_done_ns = 0;
    std::uint64_t mpp_done_ns = 0;
    std::uint64_t output_enqueue_start_ns = 0;
    std::uint64_t output_published_ns = 0;
    std::uint64_t rtsp_write_start_ns = 0;
    std::uint64_t rtsp_write_end_ns = 0;
};

class LatencyTraceStore {
public:
    LatencyTraceStore() = default;
    ~LatencyTraceStore() = default;

    LatencyTraceStore(const LatencyTraceStore&) = delete;
    LatencyTraceStore& operator=(const LatencyTraceStore&) = delete;
    LatencyTraceStore(LatencyTraceStore&&) = delete;
    LatencyTraceStore& operator=(LatencyTraceStore&&) = delete;

    void add(const FrameTiming& timing, std::uint64_t received_ns) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (session_id_ != 0 && session_id_ != timing.session_id) {
            traces_.clear();
            order_.clear();
        }
        session_id_ = timing.session_id;
        FrameTrace trace;
        trace.python = timing;
        trace.cpp_frame_received_ns = received_ns;
        traces_[timing.frame_id] = trace;
        order_.push_back(timing.frame_id);
        while (order_.size() > capacity_) {
            traces_.erase(order_.front());
            order_.pop_front();
        }
    }

    void mark_rga_done(std::uint64_t frame_id, std::uint64_t timestamp_ns) {
        update(frame_id, [timestamp_ns](FrameTrace& trace) {
            trace.rga_done_ns = timestamp_ns;
        });
    }

    void mark_mpp_done(std::uint64_t frame_id, std::uint64_t timestamp_ns) {
        update(frame_id, [timestamp_ns](FrameTrace& trace) {
            trace.mpp_done_ns = timestamp_ns;
        });
    }

    void mark_output_published(std::uint64_t frame_id,
                               std::uint64_t timestamp_ns) {
        update(frame_id, [timestamp_ns](FrameTrace& trace) {
            trace.output_published_ns = timestamp_ns;
        });
    }

    void mark_output_enqueue_start(std::uint64_t frame_id,
                                   std::uint64_t timestamp_ns) {
        update(frame_id, [timestamp_ns](FrameTrace& trace) {
            trace.output_enqueue_start_ns = timestamp_ns;
        });
    }

    void mark_rtsp_start(std::uint64_t frame_id, std::uint64_t timestamp_ns) {
        update(frame_id, [timestamp_ns](FrameTrace& trace) {
            trace.rtsp_write_start_ns = timestamp_ns;
        });
    }

    void mark_rtsp_end(std::uint64_t frame_id, std::uint64_t timestamp_ns) {
        update(frame_id, [timestamp_ns](FrameTrace& trace) {
            trace.rtsp_write_end_ns = timestamp_ns;
        });
    }

    std::string json_after(std::uint64_t after_frame_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ostringstream output;
        output << "{\"session_id\":" << session_id_ << ",\"traces\":[";
        bool first = true;
        for (std::uint64_t frame_id : order_) {
            if (frame_id <= after_frame_id) {
                continue;
            }
            const auto found = traces_.find(frame_id);
            if (found == traces_.end() || found->second.rtsp_write_end_ns == 0) {
                continue;
            }
            if (!first) {
                output << ',';
            }
            first = false;
            append_json(output, found->second);
        }
        output << "]}";
        return output.str();
    }

private:
    template <typename Operation>
    void update(std::uint64_t frame_id, Operation operation) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = traces_.find(frame_id);
        if (found != traces_.end()) {
            operation(found->second);
        }
    }

    static void append_json(std::ostringstream& output,
                            const FrameTrace& trace) {
        const FrameTiming& value = trace.python;
        output << '{'
               << "\"session_id\":" << value.session_id << ','
               << "\"frame_id\":" << value.frame_id << ','
               << "\"capture_sequence\":" << value.capture_sequence << ','
               << "\"capture_ready_ns\":" << value.capture_ready_ns << ','
               << "\"inference_start_ns\":" << value.inference_start_ns << ','
               << "\"detector_start_ns\":" << value.detector_start_ns << ','
               << "\"detector_end_ns\":" << value.detector_end_ns << ','
               << "\"landmark_start_ns\":" << value.landmark_start_ns << ','
               << "\"landmark_end_ns\":" << value.landmark_end_ns << ','
               << "\"points_ready_ns\":" << value.points_ready_ns << ','
               << "\"canvas_ready_ns\":" << value.canvas_ready_ns << ','
               << "\"cpp_frame_received_ns\":"
               << trace.cpp_frame_received_ns << ','
               << "\"rga_done_ns\":" << trace.rga_done_ns << ','
               << "\"mpp_done_ns\":" << trace.mpp_done_ns << ','
               << "\"output_enqueue_start_ns\":"
               << trace.output_enqueue_start_ns << ','
               << "\"output_published_ns\":"
               << trace.output_published_ns << ','
               << "\"rtsp_write_start_ns\":"
               << trace.rtsp_write_start_ns << ','
               << "\"rtsp_write_end_ns\":" << trace.rtsp_write_end_ns
               << '}';
    }

    static constexpr std::size_t capacity_ = 900;
    mutable std::mutex mutex_;
    std::map<std::uint64_t, FrameTrace> traces_;
    std::deque<std::uint64_t> order_;
    std::uint64_t session_id_ = 0;
};

class TimedOutput final : public vtb::Output {
public:
    TimedOutput(std::unique_ptr<vtb::Output> output,
                LatencyTraceStore& trace_store)
        : output_(std::move(output)), trace_store_(trace_store) {
        require(output_ != nullptr, "Timed output requires an output backend");
    }

    ~TimedOutput() override = default;

    TimedOutput(const TimedOutput&) = delete;
    TimedOutput& operator=(const TimedOutput&) = delete;
    TimedOutput(TimedOutput&&) = delete;
    TimedOutput& operator=(TimedOutput&&) = delete;

    bool supports(const vtb::StreamDescriptor& descriptor) const override {
        return output_->supports(descriptor);
    }

    void start(const std::vector<vtb::StreamDescriptor>& descriptors) override {
        output_->start(descriptors);
    }

    void write(const vtb::MediaPacket& packet) override {
        trace_store_.mark_rtsp_start(packet.sequence(), steady_now_ns());
        try {
            output_->write(packet);
        } catch (...) {
            trace_store_.mark_rtsp_end(packet.sequence(), steady_now_ns());
            throw;
        }
        trace_store_.mark_rtsp_end(packet.sequence(), steady_now_ns());
    }

    void request_stop() noexcept override { output_->request_stop(); }
    void stop() noexcept override { output_->stop(); }
    std::string name() const override { return output_->name(); }

private:
    std::unique_ptr<vtb::Output> output_;
    LatencyTraceStore& trace_store_;
};

class DiagnosticsServer {
public:
    DiagnosticsServer(std::uint32_t port, std::string page_path,
                      const LatencyTraceStore& trace_store)
        : port_(port), page_path_(std::move(page_path)),
          trace_store_(trace_store) {}

    ~DiagnosticsServer() { stop(); }

    DiagnosticsServer(const DiagnosticsServer&) = delete;
    DiagnosticsServer& operator=(const DiagnosticsServer&) = delete;
    DiagnosticsServer(DiagnosticsServer&&) = delete;
    DiagnosticsServer& operator=(DiagnosticsServer&&) = delete;

    void start() {
        require(port_ > 0 && port_ <= 65535,
                "Diagnostics port must be between 1 and 65535");
        std::ifstream page_file(page_path_);
        require(page_file.good(),
                "Unable to open diagnostics page: " + page_path_);
        std::ostringstream page_buffer;
        page_buffer << page_file.rdbuf();
        page_ = page_buffer.str();
        require(!page_.empty(), "Diagnostics page is empty: " + page_path_);

        UniqueFd socket_fd(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
        require(socket_fd.get() >= 0,
                "Create diagnostics socket failed: " +
                    std::string(std::strerror(errno)));
        int reuse = 1;
        require(setsockopt(socket_fd.get(), SOL_SOCKET, SO_REUSEADDR,
                           &reuse, sizeof(reuse)) == 0,
                "Set diagnostics SO_REUSEADDR failed: " +
                    std::string(std::strerror(errno)));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(static_cast<std::uint16_t>(port_));
        require(bind(socket_fd.get(), reinterpret_cast<sockaddr*>(&address),
                     sizeof(address)) == 0,
                "Bind diagnostics socket failed: " +
                    std::string(std::strerror(errno)));
        require(listen(socket_fd.get(), 16) == 0,
                "Listen diagnostics socket failed: " +
                    std::string(std::strerror(errno)));
        socket_ = std::move(socket_fd);
        stop_requested_.store(false, std::memory_order_release);
        worker_ = std::thread([this] { run(); });
    }

    void stop() noexcept {
        stop_requested_.store(true, std::memory_order_release);
        if (worker_.joinable()) {
            worker_.join();
        }
        socket_.reset();
    }

private:
    static void send_all(int fd, const std::string& data) {
        std::size_t offset = 0;
        while (offset < data.size()) {
            const ssize_t count = send(fd, data.data() + offset,
                                       data.size() - offset, MSG_NOSIGNAL);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                return;
            }
            offset += static_cast<std::size_t>(count);
        }
    }

    static std::string read_request(int fd) {
        std::string request;
        std::array<char, 1024> buffer{};
        while (request.size() < 8192) {
            const ssize_t count = recv(fd, buffer.data(), buffer.size(), 0);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                break;
            }
            request.append(buffer.data(), static_cast<std::size_t>(count));
            if (request.find("\r\n\r\n") != std::string::npos) {
                break;
            }
        }
        return request;
    }

    static std::string request_target(const std::string& request) {
        const std::size_t method_end = request.find(' ');
        if (method_end == std::string::npos ||
            request.substr(0, method_end) != "GET") {
            return {};
        }
        const std::size_t target_end = request.find(' ', method_end + 1);
        if (target_end == std::string::npos) {
            return {};
        }
        return request.substr(method_end + 1,
                              target_end - method_end - 1);
    }

    static std::uint64_t after_frame_id(const std::string& target) {
        const std::string key = "after=";
        const std::size_t position = target.find(key);
        if (position == std::string::npos) {
            return 0;
        }
        const char* first = target.data() + position + key.size();
        const char* last = target.data() + target.size();
        std::uint64_t value = 0;
        const auto parsed = std::from_chars(first, last, value, 10);
        if (parsed.ec != std::errc{}) {
            return 0;
        }
        return value;
    }

    static std::string response(int status, const std::string& status_text,
                                const std::string& content_type,
                                const std::string& body) {
        std::ostringstream output;
        output << "HTTP/1.1 " << status << ' ' << status_text << "\r\n"
               << "Content-Type: " << content_type << "\r\n"
               << "Content-Length: " << body.size() << "\r\n"
               << "Cache-Control: no-store\r\n"
               << "Access-Control-Allow-Origin: *\r\n"
               << "Connection: close\r\n\r\n"
               << body;
        return output.str();
    }

    void handle_client(int fd) const {
        const std::string request = read_request(fd);
        const std::uint64_t received_ns = steady_now_ns();
        const std::string target = request_target(request);
        if (target == "/" || target == "/index.html") {
            send_all(fd, response(200, "OK", "text/html; charset=utf-8",
                                  page_));
            return;
        }
        if (target == "/health") {
            send_all(fd, response(200, "OK", "application/json",
                                  "{\"status\":\"ok\"}"));
            return;
        }
        if (target == "/api/clock") {
            const std::uint64_t send_ns = steady_now_ns();
            const std::string body =
                "{\"rk_receive_ns\":" + std::to_string(received_ns) +
                ",\"rk_send_ns\":" + std::to_string(send_ns) + "}";
            send_all(fd, response(200, "OK", "application/json", body));
            return;
        }
        if (target.rfind("/api/traces", 0) == 0) {
            const std::string body =
                trace_store_.json_after(after_frame_id(target));
            send_all(fd, response(200, "OK", "application/json", body));
            return;
        }
        send_all(fd, response(404, "Not Found", "text/plain", "Not found"));
    }

    void run() noexcept {
        while (!stop_requested_.load(std::memory_order_acquire)) {
            pollfd descriptor{};
            descriptor.fd = socket_.get();
            descriptor.events = POLLIN;
            const int poll_result = poll(&descriptor, 1, 100);
            if (poll_result < 0 && errno == EINTR) {
                continue;
            }
            if (poll_result <= 0 || (descriptor.revents & POLLIN) == 0) {
                continue;
            }
            UniqueFd client(accept4(socket_.get(), nullptr, nullptr,
                                    SOCK_CLOEXEC));
            if (client.get() < 0) {
                continue;
            }
            try {
                handle_client(client.get());
            } catch (const std::exception& error) {
                std::cerr << "Diagnostics request failed: "
                          << error.what() << '\n';
            } catch (...) {
                std::cerr << "Diagnostics request failed: unknown error\n";
            }
        }
    }

    std::uint32_t port_;
    std::string page_path_;
    const LatencyTraceStore& trace_store_;
    std::string page_;
    UniqueFd socket_;
    std::atomic<bool> stop_requested_{false};
    std::thread worker_;
};

std::uint64_t parse_unsigned(const std::string& text,
                             std::uint64_t maximum,
                             const std::string& field) {
    std::uint64_t value = 0;
    const char* first = text.data();
    const char* last = first + text.size();
    const auto parsed = std::from_chars(first, last, value, 10);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != last ||
        value == 0 || value > maximum) {
        throw std::invalid_argument(field + " must be a positive integer");
    }
    return value;
}

std::uint32_t parse_u32(const std::string& text, const std::string& field) {
    return static_cast<std::uint32_t>(parse_unsigned(
        text, std::numeric_limits<std::uint32_t>::max(), field));
}

std::size_t parse_size(const std::string& text, const std::string& field) {
    return static_cast<std::size_t>(parse_unsigned(
        text, std::numeric_limits<std::size_t>::max(), field));
}

PublisherOptions parse_arguments(int argc, char** argv) {
    PublisherOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help") {
            std::cout
                << "Usage: vtb-mpp-output-publisher --rtsp-url URL [options]\n"
                << "  --width N --height N --fps N\n"
                << "  --bitrate N --gop N --queue N\n"
                << "  --stream-id ID --dma-heap PATH\n"
                << "  --timed-input --diagnostics-port N\n"
                << "  --diagnostics-page PATH\n";
            std::exit(0);
        }
        if (argument == "--timed-input") {
            options.timed_input = true;
            continue;
        }
        if (index + 1 >= argc) {
            throw std::invalid_argument("missing value after " + argument);
        }
        const std::string value = argv[++index];
        if (argument == "--rtsp-url") {
            options.rtsp_url = value;
        } else if (argument == "--stream-id") {
            options.stream_id = value;
        } else if (argument == "--dma-heap") {
            options.dma_heap = value;
        } else if (argument == "--width") {
            options.width = parse_u32(value, "width");
        } else if (argument == "--height") {
            options.height = parse_u32(value, "height");
        } else if (argument == "--fps") {
            options.frame_rate = parse_u32(value, "frame rate");
        } else if (argument == "--bitrate") {
            options.bitrate = parse_u32(value, "bitrate");
        } else if (argument == "--gop") {
            options.gop = parse_u32(value, "GOP");
        } else if (argument == "--queue") {
            options.queue_capacity = parse_size(value, "queue capacity");
        } else if (argument == "--diagnostics-port") {
            options.diagnostics_port = parse_u32(value, "diagnostics port");
        } else if (argument == "--diagnostics-page") {
            options.diagnostics_page = value;
        } else {
            throw std::invalid_argument("unknown argument: " + argument);
        }
    }
    if (options.rtsp_url.empty()) {
        throw std::invalid_argument("--rtsp-url is required");
    }
    if (options.stream_id.empty() || options.dma_heap.empty()) {
        throw std::invalid_argument("stream ID and DMA heap must not be empty");
    }
    if (options.width % 2 != 0 || options.height % 2 != 0) {
        throw std::invalid_argument("NV12 width and height must be even");
    }
    if (options.bitrate > static_cast<std::uint32_t>(
                              std::numeric_limits<RK_S32>::max() / 5 * 4)) {
        throw std::invalid_argument("bitrate is too large for MPP configuration");
    }
    if (options.diagnostics_port > 65535) {
        throw std::invalid_argument("diagnostics port must not exceed 65535");
    }
    if (options.diagnostics_port != 0 && options.diagnostics_page.empty()) {
        throw std::invalid_argument(
            "--diagnostics-page is required when diagnostics are enabled");
    }
    return options;
}

bool read_exact(int fd, std::uint8_t* destination, std::size_t size,
                bool allow_clean_eof) {
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t count = read(fd, destination + offset, size - offset);
        if (count == 0) {
            if (offset == 0 && allow_clean_eof) {
                return false;
            }
            throw std::runtime_error("raw BGR input ended in the middle of a frame");
        }
        if (count < 0) {
            if (errno == EINTR && stop_requested != 0) {
                return false;
            }
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(
                "read raw BGR input failed: " +
                std::string(std::strerror(errno)));
        }
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

std::uint16_t read_le16(const std::uint8_t* value) {
    return static_cast<std::uint16_t>(value[0]) |
           static_cast<std::uint16_t>(value[1]) << 8U;
}

std::uint32_t read_le32(const std::uint8_t* value) {
    std::uint32_t result = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        result |= static_cast<std::uint32_t>(value[index]) << (index * 8U);
    }
    return result;
}

std::uint64_t read_le64(const std::uint8_t* value) {
    std::uint64_t result = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        result |= static_cast<std::uint64_t>(value[index]) << (index * 8U);
    }
    return result;
}

bool read_timed_header(FrameTiming& timing, std::size_t expected_payload_size) {
    static constexpr std::size_t header_size = 104;
    std::array<std::uint8_t, header_size> header{};
    if (!read_exact(STDIN_FILENO, header.data(), header.size(), true)) {
        return false;
    }
    require(header[0] == 'V' && header[1] == 'T' &&
                header[2] == 'B' && header[3] == 'F',
            "Timed frame header magic is invalid");
    require(read_le16(header.data() + 4) == 1,
            "Timed frame header version is unsupported");
    require(read_le16(header.data() + 6) == header_size,
            "Timed frame header size is invalid");

    timing.session_id = read_le64(header.data() + 8);
    timing.frame_id = read_le64(header.data() + 16);
    timing.capture_sequence = read_le64(header.data() + 24);
    timing.capture_ready_ns = read_le64(header.data() + 32);
    timing.inference_start_ns = read_le64(header.data() + 40);
    timing.detector_start_ns = read_le64(header.data() + 48);
    timing.detector_end_ns = read_le64(header.data() + 56);
    timing.landmark_start_ns = read_le64(header.data() + 64);
    timing.landmark_end_ns = read_le64(header.data() + 72);
    timing.points_ready_ns = read_le64(header.data() + 80);
    timing.canvas_ready_ns = read_le64(header.data() + 88);
    require(read_le32(header.data() + 96) == expected_payload_size,
            "Timed frame payload size does not match the configured canvas");
    require(timing.session_id != 0 && timing.frame_id != 0,
            "Timed frame identifiers must be nonzero");
    require(timing.capture_ready_ns <= timing.inference_start_ns &&
                timing.inference_start_ns <= timing.points_ready_ns &&
                timing.points_ready_ns <= timing.canvas_ready_ns,
            "Timed frame timestamps are not monotonic");
    return true;
}

bool read_bgr_frame(MappedDmaBuffer& buffer,
                    const PublisherOptions& options,
                    std::uint32_t source_stride,
                    std::uint64_t fallback_frame_id,
                    FrameTiming& timing) {
    const std::size_t row_bytes = static_cast<std::size_t>(options.width) * 3;
    const std::size_t payload_size = row_bytes * options.height;
    if (options.timed_input) {
        if (!read_timed_header(timing, payload_size)) {
            return false;
        }
    } else {
        timing.session_id = 1;
        timing.frame_id = fallback_frame_id;
    }

    CpuWriteScope write_scope(buffer);
    const std::size_t stride_bytes = static_cast<std::size_t>(source_stride) * 3;
    for (std::uint32_t row = 0; row < options.height; ++row) {
        std::uint8_t* row_start = buffer.data() +
                                  static_cast<std::size_t>(row) * stride_bytes;
        bool allow_clean_eof = false;
        if (!options.timed_input && row == 0) {
            allow_clean_eof = true;
        }
        const bool complete = read_exact(STDIN_FILENO, row_start, row_bytes,
                                         allow_clean_eof);
        if (!complete) {
            return false;
        }
        if (stride_bytes > row_bytes) {
            std::memset(row_start + row_bytes, 0, stride_bytes - row_bytes);
        }
    }
    write_scope.finish();
    if (!options.timed_input) {
        const std::uint64_t timestamp_ns = steady_now_ns();
        timing.capture_ready_ns = timestamp_ns;
        timing.inference_start_ns = timestamp_ns;
        timing.points_ready_ns = timestamp_ns;
        timing.canvas_ready_ns = timestamp_ns;
    }
    return true;
}

vtb::OutputStatus output_status(vtb::OutputManager& manager) {
    const std::vector<vtb::OutputStatus> statuses = manager.status();
    if (statuses.size() != 1) {
        throw std::logic_error("publisher expected exactly one output");
    }
    return statuses.front();
}

void wait_until_running(vtb::OutputManager& manager) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        const vtb::OutputStatus status = output_status(manager);
        if (status.state == vtb::OutputState::running) {
            return;
        }
        if (status.state == vtb::OutputState::failed) {
            throw std::runtime_error("RTSP output failed: " + status.last_error);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const vtb::OutputStatus status = output_status(manager);
    throw std::runtime_error("RTSP output did not start: " + status.last_error);
}

void drain_output(vtb::OutputManager& manager) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        const vtb::OutputStatus status = output_status(manager);
        if (status.queue_depth == 0 &&
            status.written + status.dropped >= status.published) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("RTSP output did not drain before timeout");
}

void print_status(const vtb::OutputStatus& status,
                  std::uint64_t frames, double input_wait_ms,
                  double rga_ms, double mpp_ms, double publish_ms,
                  std::chrono::steady_clock::duration wall_time) {
    const double wall_seconds =
        std::chrono::duration<double>(wall_time).count();
    double wall_fps = 0.0;
    if (wall_seconds > 0.0) {
        wall_fps = static_cast<double>(frames) / wall_seconds;
    }
    std::cerr << "MPP publisher status: frames=" << frames
              << " wall_fps=" << wall_fps
              << " input_wait_avg_ms=" << input_wait_ms / frames
              << " rga_avg_ms=" << rga_ms / frames
              << " mpp_avg_ms=" << mpp_ms / frames
              << " publish_avg_ms=" << publish_ms / frames
              << " published=" << status.published
              << " written=" << status.written
              << " dropped=" << status.dropped
              << " errors=" << status.errors
              << " queue=" << status.queue_depth << '/'
              << status.queue_capacity << '\n';
}

int publish(const PublisherOptions& options) {
    const std::uint32_t source_stride = align_up(options.width, 16);
    const std::uint32_t nv12_stride = align_up(options.width, 16);
    const std::uint32_t nv12_vertical_stride = align_up(options.height, 16);
    const std::size_t bgr_size =
        static_cast<std::size_t>(source_stride) * options.height * 3;
    const std::size_t nv12_size =
        static_cast<std::size_t>(nv12_stride) * nv12_vertical_stride * 3 / 2;

    MappedDmaBuffer bgr_buffer(options.dma_heap, bgr_size);
    MappedDmaBuffer nv12_buffer(options.dma_heap, nv12_size);
    RgaConverter converter;
    converter.initialize(bgr_buffer, nv12_buffer, options.width, options.height,
                         source_stride, nv12_stride, nv12_vertical_stride);
    MppEncoder encoder;
    encoder.initialize(nv12_buffer, options, nv12_stride,
                       nv12_vertical_stride);

    LatencyTraceStore trace_store;
    std::unique_ptr<DiagnosticsServer> diagnostics;
    if (options.diagnostics_port != 0) {
        diagnostics = std::make_unique<DiagnosticsServer>(
            options.diagnostics_port, options.diagnostics_page, trace_store);
        diagnostics->start();
        std::cerr << "Latency diagnostics: http://0.0.0.0:"
                  << options.diagnostics_port << "/\n";
    }

    const vtb::StreamDescriptor descriptor = vtb::StreamDescriptor::video(
        options.stream_id, 1, "h264", options.width, options.height,
        options.frame_rate, 1, true, encoder.codec_configuration());

    vtb::OutputManager manager;
    manager.register_stream(descriptor);
    vtb::OutputOptions output_options;
    output_options.queue_capacity = options.queue_capacity;
    auto rtsp_output = std::make_unique<vtb::RtspOutput>(options.rtsp_url);
    manager.add("rtsp",
                std::make_unique<TimedOutput>(std::move(rtsp_output),
                                              trace_store),
                output_options);
    manager.bind("rtsp", descriptor.id());
    manager.start("rtsp");
    wait_until_running(manager);

    std::cerr << "MPP RTSP output running via OutputManager: "
              << options.rtsp_url << " stream=" << options.width << 'x'
              << options.height << ' ' << options.frame_rate
              << " fps bitrate=" << options.bitrate << '\n';

    const std::uint64_t duration_ns = 1000000000ULL / options.frame_rate;
    const auto wall_start = std::chrono::steady_clock::now();
    std::uint64_t sequence = 0;
    double input_wait_total_ms = 0.0;
    double rga_total_ms = 0.0;
    double mpp_total_ms = 0.0;
    double publish_total_ms = 0.0;
    while (stop_requested == 0) {
        const auto input_start = std::chrono::steady_clock::now();
        FrameTiming timing;
        if (!read_bgr_frame(bgr_buffer, options, source_stride, sequence + 1,
                            timing)) {
            break;
        }
        const auto input_end = std::chrono::steady_clock::now();
        const std::uint64_t input_end_ns = steady_now_ns();
        trace_store.add(timing, input_end_ns);
        converter.convert();
        const auto rga_end = std::chrono::steady_clock::now();
        trace_store.mark_rga_done(timing.frame_id, steady_now_ns());

        const std::uint64_t input_pts_ns = steady_now_ns();
        bool keyframe = false;
        std::uint64_t packet_pts_ns = input_pts_ns;
        std::uint64_t packet_dts_ns = input_pts_ns;
        auto payload = encoder.encode(input_pts_ns, keyframe,
                                      packet_pts_ns, packet_dts_ns);
        const auto mpp_end = std::chrono::steady_clock::now();
        trace_store.mark_mpp_done(timing.frame_id, steady_now_ns());

        const vtb::MediaPacket packet(
            descriptor.id(), std::move(payload), packet_pts_ns, packet_dts_ns,
            duration_ns, timing.frame_id, keyframe, false);
        trace_store.mark_output_enqueue_start(timing.frame_id,
                                              steady_now_ns());
        manager.publish(packet);
        const auto publish_end = std::chrono::steady_clock::now();
        trace_store.mark_output_published(timing.frame_id, steady_now_ns());

        ++sequence;
        input_wait_total_ms += milliseconds(input_end - input_start);
        rga_total_ms += milliseconds(rga_end - input_end);
        mpp_total_ms += milliseconds(mpp_end - rga_end);
        publish_total_ms += milliseconds(publish_end - mpp_end);
        if (sequence % 300 == 0) {
            print_status(output_status(manager), sequence, input_wait_total_ms,
                         rga_total_ms, mpp_total_ms, publish_total_ms,
                         publish_end - wall_start);
        }
    }

    drain_output(manager);
    const vtb::OutputStatus final_status = output_status(manager);
    if (sequence > 0) {
        print_status(final_status, sequence, input_wait_total_ms,
                     rga_total_ms, mpp_total_ms, publish_total_ms,
                     std::chrono::steady_clock::now() - wall_start);
    }
    manager.stop("rtsp");
    if (final_status.errors != 0) {
        return 2;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    std::signal(SIGHUP, handle_signal);
    try {
        return publish(parse_arguments(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "vtb-mpp-output-publisher: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "vtb-mpp-output-publisher: unknown failure\n";
        return 1;
    }
}
