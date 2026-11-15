#include "mpp_encoder_session.hpp"

#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

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

namespace vtb {
namespace {

constexpr RK_S32 kFixedQp = 18;

[[noreturn]] void throw_mpp_error(
    const std::string& operation,
    MPP_RET result) {
    throw std::runtime_error(
        operation + " failed: " + std::to_string(result));
}

RK_S32 checked_dimension(std::uint32_t value, const char* name) {
    if (value == 0 ||
        value > static_cast<std::uint32_t>(std::numeric_limits<RK_S32>::max())) {
        throw std::invalid_argument(
            std::string("Invalid MPP ") + name);
    }
    return static_cast<RK_S32>(value);
}

bool contains_idr(const std::uint8_t* bytes, std::size_t size) {
    if (bytes == nullptr) {
        return false;
    }
    for (std::size_t index = 0; index + 3 < size; ++index) {
        std::size_t nal_index = size;
        if (bytes[index] == 0 && bytes[index + 1] == 0 &&
            bytes[index + 2] == 1) {
            nal_index = index + 3;
        } else if (index + 4 < size && bytes[index] == 0 &&
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

class ImportedInputBuffer final {
public:
    ImportedInputBuffer(
        int dma_buf_fd,
        std::size_t size,
        std::uint64_t buffer_id,
        std::uint64_t stream_generation,
        MppBuffer buffer) noexcept
        : dma_buf_fd_(dma_buf_fd),
          size_(size),
          buffer_id_(buffer_id),
          stream_generation_(stream_generation),
          buffer_(buffer) {}

    ~ImportedInputBuffer() {
        if (buffer_ != nullptr) {
            mpp_buffer_put(buffer_);
        }
    }

    ImportedInputBuffer(const ImportedInputBuffer&) = delete;
    ImportedInputBuffer& operator=(const ImportedInputBuffer&) = delete;
    ImportedInputBuffer(ImportedInputBuffer&&) = delete;
    ImportedInputBuffer& operator=(ImportedInputBuffer&&) = delete;

    bool matches(
        int dma_buf_fd,
        std::size_t size,
        std::uint64_t buffer_id,
        std::uint64_t stream_generation) const noexcept {
        return dma_buf_fd_ == dma_buf_fd && size_ == size &&
            buffer_id_ == buffer_id &&
            stream_generation_ == stream_generation;
    }

    bool same_identity(
        std::uint64_t buffer_id,
        std::uint64_t stream_generation) const noexcept {
        return buffer_id_ == buffer_id &&
            stream_generation_ == stream_generation;
    }

    MppBuffer buffer() const noexcept { return buffer_; }

private:
    int dma_buf_fd_ = -1;
    std::size_t size_ = 0;
    std::uint64_t buffer_id_ = 0;
    std::uint64_t stream_generation_ = 0;
    MppBuffer buffer_ = nullptr;
};

}  // namespace

MppEncodedPacket::MppEncodedPacket(
    void* packet,
    std::uint64_t pts_ns,
    std::uint64_t dts_ns,
    bool keyframe) noexcept
    : packet_(packet),
      pts_ns_(pts_ns),
      dts_ns_(dts_ns),
      keyframe_(keyframe) {}

MppEncodedPacket::~MppEncodedPacket() {
    reset();
}

MppEncodedPacket::MppEncodedPacket(MppEncodedPacket&& other) noexcept
    : packet_(other.packet_),
      pts_ns_(other.pts_ns_),
      dts_ns_(other.dts_ns_),
      keyframe_(other.keyframe_) {
    other.packet_ = nullptr;
    other.pts_ns_ = 0;
    other.dts_ns_ = 0;
    other.keyframe_ = false;
}

MppEncodedPacket& MppEncodedPacket::operator=(
    MppEncodedPacket&& other) noexcept {
    if (this != &other) {
        reset();
        packet_ = other.packet_;
        pts_ns_ = other.pts_ns_;
        dts_ns_ = other.dts_ns_;
        keyframe_ = other.keyframe_;
        other.packet_ = nullptr;
        other.pts_ns_ = 0;
        other.dts_ns_ = 0;
        other.keyframe_ = false;
    }
    return *this;
}

const std::uint8_t* MppEncodedPacket::data() const noexcept {
    if (packet_ == nullptr) {
        return nullptr;
    }
    return static_cast<const std::uint8_t*>(
        mpp_packet_get_pos(static_cast<MppPacket>(packet_)));
}

std::size_t MppEncodedPacket::size() const noexcept {
    if (packet_ == nullptr) {
        return 0;
    }
    return mpp_packet_get_length(static_cast<MppPacket>(packet_));
}

std::uint64_t MppEncodedPacket::pts_ns() const noexcept {
    return pts_ns_;
}

std::uint64_t MppEncodedPacket::dts_ns() const noexcept {
    return dts_ns_;
}

bool MppEncodedPacket::keyframe() const noexcept {
    return keyframe_;
}

void MppEncodedPacket::reset() noexcept {
    if (packet_ != nullptr) {
        MppPacket packet = static_cast<MppPacket>(packet_);
        mpp_packet_deinit(&packet);
        packet_ = nullptr;
    }
}

class MppEncoderSession::Impl final {
public:
    Impl() = default;
    ~Impl() {
        shutdown();
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void initialize(
        MppVideoCodec selected_codec,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t horizontal_stride,
        std::uint32_t vertical_stride,
        std::uint32_t frames_per_second) {
        if (context_ != nullptr) {
            throw std::logic_error("MPP encoder is already initialized");
        }
        const RK_S32 mpp_width = checked_dimension(width, "width");
        const RK_S32 mpp_height = checked_dimension(height, "height");
        const RK_S32 mpp_horizontal_stride =
            checked_dimension(horizontal_stride, "horizontal stride");
        const RK_S32 mpp_vertical_stride =
            checked_dimension(vertical_stride, "vertical stride");
        const RK_S32 mpp_fps =
            checked_dimension(frames_per_second, "frame rate");

        MPP_RET result = mpp_create(&context_, &api_);
        if (result != MPP_OK) {
            throw_mpp_error("mpp_create", result);
        }
        try {
            MppPollType blocking = MPP_POLL_BLOCK;
            result = api_->control(
                context_, MPP_SET_OUTPUT_TIMEOUT, &blocking);
            if (result != MPP_OK) {
                throw_mpp_error("MPP_SET_OUTPUT_TIMEOUT", result);
            }

            MppCodingType coding_type = MPP_VIDEO_CodingAVC;
            if (selected_codec == MppVideoCodec::mjpeg) {
                coding_type = MPP_VIDEO_CodingMJPEG;
            }
            result = mpp_init(context_, MPP_CTX_ENC, coding_type);
            if (result != MPP_OK) {
                throw_mpp_error("mpp_init encoder", result);
            }

            result = mpp_enc_cfg_init(&config_);
            if (result != MPP_OK) {
                throw_mpp_error("mpp_enc_cfg_init", result);
            }
            result = api_->control(context_, MPP_ENC_GET_CFG, config_);
            if (result != MPP_OK) {
                throw_mpp_error("MPP_ENC_GET_CFG", result);
            }

            const char* common_names[] = {
                "prep:width", "prep:height", "prep:hor_stride",
                "prep:ver_stride", "prep:format", "rc:mode",
                "rc:fps_in_flex", "rc:fps_in_num", "rc:fps_in_denorm",
                "rc:fps_out_flex", "rc:fps_out_num", "rc:fps_out_denorm",
                "rc:gop", "codec:type",
            };
            const RK_S32 common_values[] = {
                mpp_width,
                mpp_height,
                mpp_horizontal_stride,
                mpp_vertical_stride,
                MPP_FMT_YUV420SP,
                MPP_ENC_RC_MODE_FIXQP,
                0,
                mpp_fps,
                1,
                0,
                mpp_fps,
                1,
                mpp_fps * 2,
                coding_type,
            };
            for (std::size_t index = 0;
                 index < sizeof(common_values) / sizeof(common_values[0]);
                 ++index) {
                set_config(common_names[index], common_values[index]);
            }

            if (selected_codec == MppVideoCodec::h264) {
                const char* h264_names[] = {
                    "h264:profile", "h264:level", "h264:cabac_en",
                    "h264:cabac_idc", "h264:trans8x8", "h264:qp_init",
                    "h264:qp_min", "h264:qp_max", "h264:qp_min_i",
                    "h264:qp_max_i",
                };
                const RK_S32 h264_values[] = {
                    100, 40, 1, 0, 1, kFixedQp, kFixedQp, kFixedQp,
                    kFixedQp, kFixedQp,
                };
                for (std::size_t index = 0;
                     index < sizeof(h264_values) / sizeof(h264_values[0]);
                     ++index) {
                    set_config(h264_names[index], h264_values[index]);
                }
            } else {
                const char* mjpeg_names[] = {
                    "jpeg:q_factor", "jpeg:qf_min", "jpeg:qf_max",
                };
                const RK_S32 mjpeg_values[] = {80, 1, 99};
                for (std::size_t index = 0;
                     index < sizeof(mjpeg_values) / sizeof(mjpeg_values[0]);
                     ++index) {
                    set_config(mjpeg_names[index], mjpeg_values[index]);
                }
            }

            result = api_->control(context_, MPP_ENC_SET_CFG, config_);
            if (result != MPP_OK) {
                throw_mpp_error("MPP_ENC_SET_CFG", result);
            }
            if (selected_codec == MppVideoCodec::h264) {
                MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
                result = api_->control(
                    context_, MPP_ENC_SET_HEADER_MODE, &header_mode);
                if (result != MPP_OK) {
                    throw_mpp_error("MPP_ENC_SET_HEADER_MODE", result);
                }
            }

            result = mpp_frame_init(&frame_);
            if (result != MPP_OK) {
                throw_mpp_error("mpp_frame_init", result);
            }
            mpp_frame_set_width(frame_, mpp_width);
            mpp_frame_set_height(frame_, mpp_height);
            mpp_frame_set_hor_stride(frame_, mpp_horizontal_stride);
            mpp_frame_set_ver_stride(frame_, mpp_vertical_stride);
            mpp_frame_set_fmt(frame_, MPP_FMT_YUV420SP);
            codec_ = selected_codec;
        } catch (...) {
            shutdown();
            throw;
        }
    }

    std::vector<std::uint8_t> h264_codec_configuration() {
        if (context_ == nullptr || codec_ != MppVideoCodec::h264) {
            throw std::logic_error(
                "H.264 codec configuration is unavailable");
        }
        std::vector<std::uint8_t> storage(4096);
        MppPacket packet = nullptr;
        MPP_RET result =
            mpp_packet_init(&packet, storage.data(), storage.size());
        if (result != MPP_OK || packet == nullptr) {
            throw_mpp_error("mpp_packet_init H.264 header", result);
        }
        try {
            mpp_packet_set_length(packet, 0);
            result = api_->control(context_, MPP_ENC_GET_HDR_SYNC, packet);
            if (result != MPP_OK) {
                throw_mpp_error("MPP_ENC_GET_HDR_SYNC", result);
            }
            const auto* position = static_cast<const std::uint8_t*>(
                mpp_packet_get_pos(packet));
            const std::size_t length = mpp_packet_get_length(packet);
            if (position == nullptr || length == 0) {
                throw std::runtime_error("MPP returned an empty H.264 header");
            }
            std::vector<std::uint8_t> result_bytes(position, position + length);
            mpp_packet_deinit(&packet);
            return result_bytes;
        } catch (...) {
            mpp_packet_deinit(&packet);
            throw;
        }
    }

    MppEncodedPacket encode(
        int input_fd,
        std::size_t input_size,
        std::uint64_t buffer_id,
        std::uint64_t stream_generation,
        std::uint64_t pts_ns,
        bool end_of_stream) {
        if (context_ == nullptr || frame_ == nullptr) {
            throw std::logic_error("MPP encoder is not initialized");
        }
        if (input_fd < 0 || input_size == 0 || buffer_id == 0 ||
            stream_generation == 0) {
            throw std::invalid_argument("MPP input DMA-BUF is invalid");
        }
        MppBuffer input_buffer = input_buffer_for(
            input_fd,
            input_size,
            buffer_id,
            stream_generation);
        mpp_frame_set_buffer(frame_, input_buffer);
        mpp_frame_set_pts(frame_, static_cast<RK_S64>(pts_ns));
        RK_U32 eos = 0;
        if (end_of_stream) {
            eos = 1;
        }
        mpp_frame_set_eos(frame_, eos);

        MPP_RET result = api_->encode_put_frame(context_, frame_);
        if (result != MPP_OK) {
            throw_mpp_error("encode_put_frame", result);
        }
        MppPacket packet = nullptr;
        result = api_->encode_get_packet(context_, &packet);
        if (result != MPP_OK || packet == nullptr) {
            throw_mpp_error("encode_get_packet", result);
        }

        const auto* bytes = static_cast<const std::uint8_t*>(
            mpp_packet_get_pos(packet));
        const std::size_t length = mpp_packet_get_length(packet);
        if (bytes == nullptr || length == 0) {
            mpp_packet_deinit(&packet);
            throw std::runtime_error("MPP returned an empty encoded packet");
        }

        std::uint64_t packet_pts_ns = pts_ns;
        const RK_S64 packet_pts = mpp_packet_get_pts(packet);
        if (packet_pts >= 0) {
            packet_pts_ns = static_cast<std::uint64_t>(packet_pts);
        }
        std::uint64_t packet_dts_ns = pts_ns;
        const RK_S64 packet_dts = mpp_packet_get_dts(packet);
        if (packet_dts >= 0) {
            packet_dts_ns = static_cast<std::uint64_t>(packet_dts);
        }
        bool keyframe = false;
        if (codec_ == MppVideoCodec::mjpeg) {
            keyframe = true;
        } else {
            keyframe = contains_idr(bytes, length);
        }
        encoded_bytes_ += length;
        return MppEncodedPacket(
            packet, packet_pts_ns, packet_dts_ns, keyframe);
    }

    void request_idr() {
        if (context_ == nullptr || codec_ != MppVideoCodec::h264) {
            throw std::logic_error(
                "IDR can only be requested from an initialized H.264 encoder");
        }
        const MPP_RET result =
            api_->control(context_, MPP_ENC_SET_IDR_FRAME, nullptr);
        if (result != MPP_OK) {
            throw_mpp_error("MPP_ENC_SET_IDR_FRAME", result);
        }
    }

    void shutdown() noexcept {
        if (frame_ != nullptr) {
            mpp_frame_deinit(&frame_);
            frame_ = nullptr;
        }
        input_buffers_.clear();
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

    std::size_t encoded_bytes() const noexcept {
        return encoded_bytes_;
    }

private:
    MppBuffer input_buffer_for(
        int input_fd,
        std::size_t input_size,
        std::uint64_t buffer_id,
        std::uint64_t stream_generation) {
        for (const auto& input : input_buffers_) {
            if (input->matches(
                    input_fd,
                    input_size,
                    buffer_id,
                    stream_generation)) {
                return input->buffer();
            }
            if (input->same_identity(buffer_id, stream_generation)) {
                throw std::invalid_argument(
                    "MPP DMA-BUF identity changed without stream reconfiguration");
            }
        }

        MppBufferInfo input_info{};
        input_info.type = MPP_BUFFER_TYPE_EXT_DMA;
        input_info.size = input_size;
        input_info.fd = input_fd;
        input_info.index = -1;
        MppBuffer imported = nullptr;
        const MPP_RET result = mpp_buffer_import(&imported, &input_info);
        if (result != MPP_OK || imported == nullptr) {
            throw_mpp_error("mpp_buffer_import NV12 DMA-BUF", result);
        }
        try {
            input_buffers_.push_back(std::make_unique<ImportedInputBuffer>(
                input_fd,
                input_size,
                buffer_id,
                stream_generation,
                imported));
        } catch (...) {
            mpp_buffer_put(imported);
            throw;
        }
        return imported;
    }

    void set_config(const char* name, RK_S32 value) {
        const MPP_RET result = mpp_enc_cfg_set_s32(config_, name, value);
        if (result != MPP_OK) {
            throw_mpp_error(
                std::string("mpp_enc_cfg_set_s32 ") + name, result);
        }
    }

    MppCtx context_ = nullptr;
    MppApi* api_ = nullptr;
    MppEncCfg config_ = nullptr;
    std::vector<std::unique_ptr<ImportedInputBuffer>> input_buffers_;
    MppFrame frame_ = nullptr;
    std::size_t encoded_bytes_ = 0;
    MppVideoCodec codec_ = MppVideoCodec::h264;
};

MppEncoderSession::MppEncoderSession()
    : impl_(std::make_unique<Impl>()) {}

MppEncoderSession::~MppEncoderSession() = default;

void MppEncoderSession::initialize(
    MppVideoCodec codec,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t horizontal_stride,
    std::uint32_t vertical_stride,
    std::uint32_t frames_per_second) {
    impl_->initialize(
        codec,
        width,
        height,
        horizontal_stride,
        vertical_stride,
        frames_per_second);
}

std::vector<std::uint8_t>
MppEncoderSession::h264_codec_configuration() {
    return impl_->h264_codec_configuration();
}

MppEncodedPacket MppEncoderSession::encode(
    int input_fd,
    std::size_t input_size,
    std::uint64_t buffer_id,
    std::uint64_t stream_generation,
    std::uint64_t pts_ns,
    bool end_of_stream) {
    return impl_->encode(
        input_fd,
        input_size,
        buffer_id,
        stream_generation,
        pts_ns,
        end_of_stream);
}

void MppEncoderSession::request_idr() {
    impl_->request_idr();
}

void MppEncoderSession::shutdown() noexcept {
    impl_->shutdown();
}

std::size_t MppEncoderSession::encoded_bytes() const noexcept {
    return impl_->encoded_bytes();
}

}  // namespace vtb
