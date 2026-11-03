#define MODULE_TAG "openvt_dmabuf_capture"

#include "openvt_dmabuf_capture.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/memory.h"
#include "core/os/mutex.h"
#include "core/os/os.h"
#include "core/os/thread.h"
#include "core/string/print_string.h"
#include "platform/linuxbsd/platform_egl.h"
#include "platform/linuxbsd/platform_gl.h"
#include "servers/rendering/rendering_server.h"
#include "vtb/instrumentation/timer.hpp"

#include <drm/drm_fourcc.h>
#include <linux/dma-heap.h>
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

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr const char *DMA_HEAP_PATH = "/dev/dma_heap/system-uncached";
constexpr EGLenum EGL_LINUX_DMA_BUF_TARGET = 0x3270;
constexpr EGLint EGL_LINUX_DRM_FOURCC_ATTRIBUTE = 0x3271;
constexpr EGLint EGL_DMA_BUF_PLANE0_FD_ATTRIBUTE = 0x3272;
constexpr EGLint EGL_DMA_BUF_PLANE0_OFFSET_ATTRIBUTE = 0x3273;
constexpr EGLint EGL_DMA_BUF_PLANE0_PITCH_ATTRIBUTE = 0x3274;
constexpr int FRAME_SLOT_COUNT = 3;

using EglCreateImageKhrProc = EGLImageKHR (*)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
using EglDestroyImageKhrProc = EGLBoolean (*)(EGLDisplay, EGLImageKHR);
using GlEglImageTargetTexture2DProc = void (*)(GLenum, void *);

String errno_message(const String &p_prefix) {
	return p_prefix + ": " + String::utf8(std::strerror(errno));
}

uint64_t monotonic_now_ns() {
	const auto now = std::chrono::steady_clock::now().time_since_epoch();
	return static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

class UniqueFd {
public:
	UniqueFd() = default;
	explicit UniqueFd(int p_fd) : fd(p_fd) {}
	~UniqueFd() { reset(); }

	UniqueFd(const UniqueFd &) = delete;
	UniqueFd &operator=(const UniqueFd &) = delete;

	UniqueFd(UniqueFd &&p_other) noexcept : fd(p_other.release()) {}
	UniqueFd &operator=(UniqueFd &&p_other) noexcept {
		if (this != &p_other) {
			reset(p_other.release());
		}
		return *this;
	}

	int get() const { return fd; }

	int release() {
		int released = fd;
		fd = -1;
		return released;
	}

	void reset(int p_replacement = -1) {
		if (fd >= 0) {
			close(fd);
		}
		fd = p_replacement;
	}

private:
	int fd = -1;
};

class DmaBuffer {
public:
	DmaBuffer() = default;
	~DmaBuffer() = default;

	DmaBuffer(const DmaBuffer &) = delete;
	DmaBuffer &operator=(const DmaBuffer &) = delete;
	DmaBuffer(DmaBuffer &&) = delete;
	DmaBuffer &operator=(DmaBuffer &&) = delete;

	Error allocate(const String &p_heap_path, size_t p_size, String &r_error) {
		if (buffer_fd.get() >= 0) {
			r_error = "DMA buffer is already allocated";
			return ERR_ALREADY_IN_USE;
		}

		CharString heap_path_utf8 = p_heap_path.utf8();
		UniqueFd heap_fd(open(heap_path_utf8.get_data(), O_RDONLY | O_CLOEXEC));
		if (heap_fd.get() < 0) {
			r_error = errno_message("open DMA heap failed: " + p_heap_path);
			return ERR_CANT_OPEN;
		}

		dma_heap_allocation_data allocation{};
		allocation.len = p_size;
		allocation.fd_flags = O_RDWR | O_CLOEXEC;
		allocation.heap_flags = 0;
		if (ioctl(heap_fd.get(), DMA_HEAP_IOCTL_ALLOC, &allocation) != 0) {
			r_error = errno_message("DMA_HEAP_IOCTL_ALLOC failed");
			return ERR_OUT_OF_MEMORY;
		}

		buffer_fd.reset(static_cast<int>(allocation.fd));
		buffer_size = p_size;
		return OK;
	}

	int get_fd() const { return buffer_fd.get(); }
	size_t get_size() const { return buffer_size; }

private:
	UniqueFd buffer_fd;
	size_t buffer_size = 0;
};

class ExternalDmaBufTarget {
public:
	ExternalDmaBufTarget() = default;
	~ExternalDmaBufTarget() { shutdown(); }

	ExternalDmaBufTarget(const ExternalDmaBufTarget &) = delete;
	ExternalDmaBufTarget &operator=(const ExternalDmaBufTarget &) = delete;
	ExternalDmaBufTarget(ExternalDmaBufTarget &&) = delete;
	ExternalDmaBufTarget &operator=(ExternalDmaBufTarget &&) = delete;

	Error initialize(const DmaBuffer &p_buffer, int p_width, int p_height, int p_stride, String &r_error) {
		display = eglGetCurrentDisplay();
		if (display == EGL_NO_DISPLAY || eglGetCurrentContext() == EGL_NO_CONTEXT) {
			r_error = "Godot render thread has no current EGL display/context";
			return ERR_UNCONFIGURED;
		}

		create_image = reinterpret_cast<EglCreateImageKhrProc>(eglGetProcAddress("eglCreateImageKHR"));
		destroy_image = reinterpret_cast<EglDestroyImageKhrProc>(eglGetProcAddress("eglDestroyImageKHR"));
		image_target_texture = reinterpret_cast<GlEglImageTargetTexture2DProc>(
				eglGetProcAddress("glEGLImageTargetTexture2DOES"));
		if (create_image == nullptr || destroy_image == nullptr || image_target_texture == nullptr) {
			r_error = "required EGLImage entry points are unavailable";
			return ERR_UNAVAILABLE;
		}

		const EGLint image_attributes[] = {
			EGL_WIDTH, p_width,
			EGL_HEIGHT, p_height,
			EGL_LINUX_DRM_FOURCC_ATTRIBUTE, static_cast<EGLint>(DRM_FORMAT_ABGR8888),
			EGL_DMA_BUF_PLANE0_FD_ATTRIBUTE, p_buffer.get_fd(),
			EGL_DMA_BUF_PLANE0_OFFSET_ATTRIBUTE, 0,
			EGL_DMA_BUF_PLANE0_PITCH_ATTRIBUTE, p_stride,
			EGL_NONE,
		};

		image = create_image(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_TARGET, nullptr, image_attributes);
		if (image == nullptr) {
			r_error = "eglCreateImageKHR failed for output DMA-BUF";
			return ERR_CANT_CREATE;
		}

		glGenTextures(1, &texture);
		glBindTexture(GL_TEXTURE_2D, texture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		image_target_texture(GL_TEXTURE_2D, image);

		glGenFramebuffers(1, &framebuffer);
		glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
		GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		if (status != GL_FRAMEBUFFER_COMPLETE) {
			r_error = "DMA-BUF output framebuffer is incomplete: " + itos(static_cast<int64_t>(status));
			return ERR_CANT_CREATE;
		}

		return OK;
	}

	void shutdown() {
		if (framebuffer != 0) {
			glDeleteFramebuffers(1, &framebuffer);
			framebuffer = 0;
		}
		if (texture != 0) {
			glDeleteTextures(1, &texture);
			texture = 0;
		}
		if (image != nullptr && destroy_image != nullptr && display != EGL_NO_DISPLAY) {
			destroy_image(display, image);
			image = nullptr;
		}
	}

	GLuint get_framebuffer() const { return framebuffer; }

private:
	EGLDisplay display = EGL_NO_DISPLAY;
	EGLImageKHR image = nullptr;
	EglCreateImageKhrProc create_image = nullptr;
	EglDestroyImageKhrProc destroy_image = nullptr;
	GlEglImageTargetTexture2DProc image_target_texture = nullptr;
	GLuint texture = 0;
	GLuint framebuffer = 0;
};

class RgaConverter {
public:
	RgaConverter() = default;
	~RgaConverter() { shutdown(); }

	RgaConverter(const RgaConverter &) = delete;
	RgaConverter &operator=(const RgaConverter &) = delete;
	RgaConverter(RgaConverter &&) = delete;
	RgaConverter &operator=(RgaConverter &&) = delete;

	Error initialize(const DmaBuffer &p_source, const DmaBuffer &p_destination, int p_width, int p_height,
			int p_rgba_stride, int p_nv12_stride, int p_nv12_vertical_stride, String &r_error) {
		source_handle = importbuffer_fd(p_source.get_fd(), static_cast<int>(p_source.get_size()));
		if (source_handle == 0) {
			r_error = "RGA source DMA-BUF import failed";
			return ERR_CANT_CREATE;
		}

		destination_handle = importbuffer_fd(p_destination.get_fd(), static_cast<int>(p_destination.get_size()));
		if (destination_handle == 0) {
			r_error = "RGA destination DMA-BUF import failed";
			return ERR_CANT_CREATE;
		}

		source_buffer = wrapbuffer_handle_t(source_handle, p_width, p_height, p_rgba_stride / 4, p_height,
				RK_FORMAT_RGBA_8888);
		destination_buffer = wrapbuffer_handle_t(destination_handle, p_width, p_height, p_nv12_stride,
				p_nv12_vertical_stride, RK_FORMAT_YCbCr_420_SP);

		im_rect source_rect{};
		im_rect destination_rect{};
		rga_buffer_t pattern_buffer{};
		im_rect pattern_rect{};
		IM_STATUS status = imcheck_t(source_buffer, destination_buffer, pattern_buffer, source_rect,
				destination_rect, pattern_rect, 0);
		if (status != IM_STATUS_NOERROR && status != IM_STATUS_SUCCESS) {
			r_error = "RGA imcheck failed: " + String::utf8(imStrError_t(status));
			return ERR_INVALID_DATA;
		}

		return OK;
	}

	Error convert(String &r_error) {
		IM_STATUS status = imcvtcolor_t(source_buffer, destination_buffer, RK_FORMAT_RGBA_8888,
				RK_FORMAT_YCbCr_420_SP, IM_RGB_TO_YUV_BT601_LIMIT, 1);
		if (status != IM_STATUS_SUCCESS) {
			r_error = "RGA imcvtcolor failed: " + String::utf8(imStrError_t(status));
			return FAILED;
		}
		return OK;
	}

	void shutdown() {
		if (destination_handle != 0) {
			releasebuffer_handle(destination_handle);
			destination_handle = 0;
		}
		if (source_handle != 0) {
			releasebuffer_handle(source_handle);
			source_handle = 0;
		}
	}

private:
	rga_buffer_handle_t source_handle = 0;
	rga_buffer_handle_t destination_handle = 0;
	rga_buffer_t source_buffer{};
	rga_buffer_t destination_buffer{};
};

class MppPacketLease final {
public:
	MppPacketLease() = default;
	explicit MppPacketLease(MppPacket p_packet) : packet(p_packet) {}
	~MppPacketLease() { reset(); }

	MppPacketLease(const MppPacketLease &) = delete;
	MppPacketLease &operator=(const MppPacketLease &) = delete;

	MppPacketLease(MppPacketLease &&p_other) noexcept : packet(p_other.packet) {
		p_other.packet = nullptr;
	}

	MppPacketLease &operator=(MppPacketLease &&p_other) noexcept {
		if (this != &p_other) {
			reset();
			packet = p_other.packet;
			p_other.packet = nullptr;
		}
		return *this;
	}

	const uint8_t *data() const {
		return static_cast<const uint8_t *>(mpp_packet_get_pos(packet));
	}

	size_t size() const { return mpp_packet_get_length(packet); }

private:
	void reset() noexcept {
		if (packet != nullptr) {
			mpp_packet_deinit(&packet);
		}
	}

	MppPacket packet = nullptr;
};

class MppEncoder {
public:
	static constexpr RK_S32 FIXED_QP = 18;

	MppEncoder() = default;
	~MppEncoder() { shutdown(); }

	MppEncoder(const MppEncoder &) = delete;
	MppEncoder &operator=(const MppEncoder &) = delete;
	MppEncoder(MppEncoder &&) = delete;
	MppEncoder &operator=(MppEncoder &&) = delete;

	Error initialize(const DmaBuffer &p_input, int p_width, int p_height,
			int p_stride, int p_vertical_stride, int p_fps, String &r_error) {
		MPP_RET result = mpp_create(&context, &api);
		if (result != MPP_OK) {
			r_error = "mpp_create failed: " + itos(result);
			return ERR_CANT_CREATE;
		}

		MppPollType blocking = MPP_POLL_BLOCK;
		result = api->control(context, MPP_SET_OUTPUT_TIMEOUT, &blocking);
		if (result != MPP_OK) {
			r_error = "MPP_SET_OUTPUT_TIMEOUT failed: " + itos(result);
			return FAILED;
		}

		result = mpp_init(context, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);
		if (result != MPP_OK) {
			r_error = "mpp_init AVC encoder failed: " + itos(result);
			return ERR_CANT_CREATE;
		}

		result = mpp_enc_cfg_init(&config);
		if (result != MPP_OK) {
			r_error = "mpp_enc_cfg_init failed: " + itos(result);
			return ERR_CANT_CREATE;
		}
		result = api->control(context, MPP_ENC_GET_CFG, config);
		if (result != MPP_OK) {
			r_error = "MPP_ENC_GET_CFG failed: " + itos(result);
			return FAILED;
		}

		const char *config_names[] = {
			"prep:width", "prep:height", "prep:hor_stride", "prep:ver_stride", "prep:format",
			"rc:mode", "rc:fps_in_flex", "rc:fps_in_num", "rc:fps_in_denorm", "rc:fps_out_flex",
			"rc:fps_out_num", "rc:fps_out_denorm", "rc:gop", "codec:type", "h264:profile",
			"h264:level", "h264:cabac_en", "h264:cabac_idc", "h264:trans8x8", "h264:qp_init",
			"h264:qp_min", "h264:qp_max", "h264:qp_min_i", "h264:qp_max_i",
		};
		RK_S32 config_values[] = {
			p_width, p_height, p_stride, p_vertical_stride, MPP_FMT_YUV420SP,
			MPP_ENC_RC_MODE_FIXQP, 0, p_fps, 1, 0,
			p_fps, 1, p_fps * 2, MPP_VIDEO_CodingAVC, 100,
			40, 1, 0, 1, FIXED_QP,
			FIXED_QP, FIXED_QP, FIXED_QP, FIXED_QP,
		};
		for (uint32_t i = 0; i < sizeof(config_values) / sizeof(config_values[0]); i++) {
			Error error = set_config(config_names[i], config_values[i], r_error);
			if (error != OK) {
				return error;
			}
		}

		result = api->control(context, MPP_ENC_SET_CFG, config);
		if (result != MPP_OK) {
			r_error = "MPP_ENC_SET_CFG failed: " + itos(result);
			return FAILED;
		}
		print_line(vformat("OPENVT_MPP_CONFIG mode=fixqp qp=%d gop=%d", FIXED_QP, p_fps * 2));

		MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
		result = api->control(context, MPP_ENC_SET_HEADER_MODE, &header_mode);
		if (result != MPP_OK) {
			r_error = "MPP_ENC_SET_HEADER_MODE failed: " + itos(result);
			return FAILED;
		}

		MppBufferInfo input_info{};
		input_info.type = MPP_BUFFER_TYPE_EXT_DMA;
		input_info.size = p_input.get_size();
		input_info.fd = p_input.get_fd();
		input_info.index = -1;
		result = mpp_buffer_import(&input_buffer, &input_info);
		if (result != MPP_OK) {
			r_error = "mpp_buffer_import NV12 DMA-BUF failed: " + itos(result);
			return ERR_CANT_CREATE;
		}

		result = mpp_frame_init(&frame);
		if (result != MPP_OK) {
			r_error = "mpp_frame_init failed: " + itos(result);
			return ERR_CANT_CREATE;
		}
		mpp_frame_set_width(frame, p_width);
		mpp_frame_set_height(frame, p_height);
		mpp_frame_set_hor_stride(frame, p_stride);
		mpp_frame_set_ver_stride(frame, p_vertical_stride);
		mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
		mpp_frame_set_buffer(frame, input_buffer);
		return OK;
	}

	Error codec_configuration(std::vector<uint8_t> &r_configuration, String &r_error) {
		std::vector<uint8_t> storage(4096);
		MppPacket packet = nullptr;
		MPP_RET result = mpp_packet_init(&packet, storage.data(), storage.size());
		if (result != MPP_OK || packet == nullptr) {
			r_error = "mpp_packet_init for AVC header failed: " + itos(result);
			return ERR_CANT_CREATE;
		}
		mpp_packet_set_length(packet, 0);
		result = api->control(context, MPP_ENC_GET_HDR_SYNC, packet);
		if (result != MPP_OK) {
			mpp_packet_deinit(&packet);
			r_error = "MPP_ENC_GET_HDR_SYNC failed: " + itos(result);
			return FAILED;
		}
		void *position = mpp_packet_get_pos(packet);
		size_t length = mpp_packet_get_length(packet);
		if (position == nullptr || length == 0) {
			mpp_packet_deinit(&packet);
			r_error = "MPP returned an empty AVC header";
			return ERR_INVALID_DATA;
		}
		const uint8_t *first = static_cast<const uint8_t *>(position);
		r_configuration.assign(first, first + length);
		mpp_packet_deinit(&packet);
		return OK;
	}

	Error encode_frame(uint64_t p_pts_ns, bool p_end_of_stream,
			MppPacketLease &r_packet,
			uint64_t &r_pts_ns, uint64_t &r_dts_ns, bool &r_keyframe, String &r_error) {
		r_packet = MppPacketLease();
		mpp_frame_set_pts(frame, static_cast<RK_S64>(p_pts_ns));
		RK_U32 eos_value = 0;
		if (p_end_of_stream) {
			eos_value = 1;
		}
		mpp_frame_set_eos(frame, eos_value);

		MPP_RET result = api->encode_put_frame(context, frame);
		if (result != MPP_OK) {
			r_error = "encode_put_frame failed: " + itos(result);
			return FAILED;
		}

		MppPacket packet = nullptr;
		result = api->encode_get_packet(context, &packet);
		if (result != MPP_OK || packet == nullptr) {
			r_error = "encode_get_packet failed: " + itos(result);
			return FAILED;
		}
		MppPacketLease packet_lease(packet);

		const uint8_t *first = packet_lease.data();
		size_t length = packet_lease.size();
		if (first == nullptr || length == 0) {
			r_error = "MPP returned an empty packet";
			return ERR_INVALID_DATA;
		}

		r_keyframe = contains_idr(first, length);
		RK_S64 packet_pts = mpp_packet_get_pts(packet);
		RK_S64 packet_dts = mpp_packet_get_dts(packet);
		r_pts_ns = p_pts_ns;
		if (packet_pts >= 0) {
			r_pts_ns = static_cast<uint64_t>(packet_pts);
		}
		r_dts_ns = p_pts_ns;
		if (packet_dts >= 0) {
			r_dts_ns = static_cast<uint64_t>(packet_dts);
		}
		encoded_bytes += length;
		r_packet = std::move(packet_lease);
		return OK;
	}

	void shutdown() {
		if (frame != nullptr) {
			mpp_frame_deinit(&frame);
		}
		if (input_buffer != nullptr) {
			mpp_buffer_put(input_buffer);
			input_buffer = nullptr;
		}
		if (config != nullptr) {
			mpp_enc_cfg_deinit(config);
			config = nullptr;
		}
		if (context != nullptr) {
			mpp_destroy(context);
			context = nullptr;
		}
	}

	size_t get_encoded_bytes() const { return encoded_bytes; }

private:
	Error set_config(const char *p_name, RK_S32 p_value, String &r_error) {
		MPP_RET result = mpp_enc_cfg_set_s32(config, p_name, p_value);
		if (result != MPP_OK) {
			r_error = "mpp_enc_cfg_set_s32 failed for " + String::utf8(p_name) + ": " + itos(result);
			return FAILED;
		}
		return OK;
	}

	static bool contains_idr(const uint8_t *p_bytes, size_t p_size) {
		if (p_bytes == nullptr) {
			return false;
		}
		for (size_t index = 0; index + 3 < p_size; index++) {
			size_t nal_index = p_size;
			if (p_bytes[index] == 0 && p_bytes[index + 1] == 0 && p_bytes[index + 2] == 1) {
				nal_index = index + 3;
			} else if (index + 4 < p_size && p_bytes[index] == 0 &&
					p_bytes[index + 1] == 0 && p_bytes[index + 2] == 0 && p_bytes[index + 3] == 1) {
				nal_index = index + 4;
			}
			if (nal_index < p_size && (p_bytes[nal_index] & 0x1fU) == 5U) {
				return true;
			}
		}
		return false;
	}

	MppCtx context = nullptr;
	MppApi *api = nullptr;
	MppEncCfg config = nullptr;
	MppBuffer input_buffer = nullptr;
	MppFrame frame = nullptr;
	size_t encoded_bytes = 0;
};

class FrameSlot {
public:
	enum class State {
		FREE,
		RESERVED,
		READY,
		CONSUMING,
	};

	FrameSlot() = default;
	~FrameSlot() = default;

	FrameSlot(const FrameSlot &) = delete;
	FrameSlot &operator=(const FrameSlot &) = delete;
	FrameSlot(FrameSlot &&) = delete;
	FrameSlot &operator=(FrameSlot &&) = delete;

	Error initialize(int p_index, const DmaBuffer &p_nv12_buffer, int p_width, int p_height,
			int p_rgba_stride, int p_nv12_stride, int p_nv12_vertical_stride, size_t p_rgba_size,
			String &r_error) {
		index = p_index;
		Error error = rgba_buffer.allocate(DMA_HEAP_PATH, p_rgba_size, r_error);
		if (error != OK) {
			return error;
		}
		error = dma_target.initialize(rgba_buffer, p_width, p_height, p_rgba_stride, r_error);
		if (error != OK) {
			return error;
		}
		error = rga_converter.initialize(rgba_buffer, p_nv12_buffer, p_width, p_height, p_rgba_stride,
				p_nv12_stride, p_nv12_vertical_stride, r_error);
		if (error != OK) {
			return error;
		}
		return OK;
	}

	int get_index() const { return index; }
	int get_rgba_fd() const { return rgba_buffer.get_fd(); }
	GLuint get_framebuffer() const { return dma_target.get_framebuffer(); }
	RgaConverter &get_rga_converter() { return rga_converter; }

	State state = State::FREE;
	int frame_index = -1;
	bool end_of_stream = false;
	uint64_t ready_usec = 0;
	uint64_t ready_ns = 0;

private:
	int index = -1;
	DmaBuffer rgba_buffer;
	ExternalDmaBufTarget dma_target;
	RgaConverter rga_converter;
};

} // namespace

class OpenVTDmaBufPipeline {
public:
	OpenVTDmaBufPipeline() :
			producer_wait_timer(timers.register_timer("live2d.producer_wait")),
			gpu_copy_timer(timers.register_timer("live2d.gpu_copy")),
			queue_wait_timer(timers.register_timer("live2d.media_queue_wait")),
			rga_timer(timers.register_timer("live2d.rga")),
			mpp_timer(timers.register_timer("live2d.mpp")),
			publish_timer(timers.register_timer("live2d.output_publish_call")),
			media_timer(timers.register_timer("live2d.media_total")) {}
	~OpenVTDmaBufPipeline() { shutdown(); }

	OpenVTDmaBufPipeline(const OpenVTDmaBufPipeline &) = delete;
	OpenVTDmaBufPipeline &operator=(const OpenVTDmaBufPipeline &) = delete;
	OpenVTDmaBufPipeline(OpenVTDmaBufPipeline &&) = delete;
	OpenVTDmaBufPipeline &operator=(OpenVTDmaBufPipeline &&) = delete;

	Error initialize(const vtb::EncodedVideoSink &p_sink, int p_width, int p_height, int p_fps, int p_total_frames,
			String &r_error) {
		width = p_width;
		height = p_height;
		fps = p_fps;
		total_frames = p_total_frames;
		rgba_stride = width * 4;
		nv12_stride = width;
		nv12_vertical_stride = (height + 15) & ~15;

		size_t rgba_size = static_cast<size_t>(rgba_stride) * height;
		size_t nv12_size = static_cast<size_t>(nv12_stride) * nv12_vertical_stride * 3 / 2;

		Error error = nv12_buffer.allocate(DMA_HEAP_PATH, nv12_size, r_error);
		if (error != OK) {
			return error;
		}

		for (int i = 0; i < FRAME_SLOT_COUNT; i++) {
			slots[i] = memnew(FrameSlot);
			error = slots[i]->initialize(i, nv12_buffer, width, height, rgba_stride, nv12_stride,
					nv12_vertical_stride, rgba_size, r_error);
			if (error != OK) {
				return error;
			}
			free_slot_indices.push_back(i);
		}
		free_slots_available.post(FRAME_SLOT_COUNT);

		glGenFramebuffers(1, &source_framebuffer);
		if (source_framebuffer == 0) {
			r_error = "failed to allocate source framebuffer";
			return ERR_CANT_CREATE;
		}

		error = mpp_encoder.initialize(nv12_buffer, width, height, nv12_stride,
				nv12_vertical_stride, fps, r_error);
		if (error != OK) {
			return error;
		}
		std::vector<uint8_t> codec_configuration;
		error = mpp_encoder.codec_configuration(codec_configuration, r_error);
		if (error != OK) {
			return error;
		}
		output_sink = p_sink;
		vtb::H264StreamFormat format;
		format.width = static_cast<uint32_t>(width);
		format.height = static_cast<uint32_t>(height);
		format.fps = static_cast<uint32_t>(fps);
		format.codec_configuration = codec_configuration.data();
		format.codec_configuration_size = codec_configuration.size();
		if (output_sink.configure_h264(output_sink.context, &format) != 0) {
			r_error = "host rejected H.264 stream configuration";
			return ERR_CANT_CONNECT;
		}

		accepting_frames = true;
		initialized = true;
#ifdef THREADS_ENABLED
		Thread::ID thread_id = media_thread.start(media_thread_entry, this);
		if (thread_id == Thread::UNASSIGNED_ID) {
			r_error = "failed to start DMA-BUF media worker";
			return ERR_CANT_CREATE;
		}
#else
		r_error = "Godot was built without thread support";
		return ERR_UNAVAILABLE;
#endif

		start_usec = OS::get_singleton()->get_ticks_usec();
		print_line(vformat("OPENVT_DMABUF_CONFIG width=%d height=%d fps=%d frames=%d slots=%d nv12_fd=%d rgba_stride=%d nv12_stride=%d nv12_vertical_stride=%d heap=%s",
				width, height, fps, total_frames, FRAME_SLOT_COUNT, nv12_buffer.get_fd(), rgba_stride,
				nv12_stride, nv12_vertical_stride, DMA_HEAP_PATH));
		for (int i = 0; i < FRAME_SLOT_COUNT; i++) {
			print_line(vformat("OPENVT_DMABUF_SLOT slot=%d rgba_fd=%d", i, slots[i]->get_rgba_fd()));
		}
		print_line("OPENVT_DMABUF_LAYOUT source=Godot_SubViewport_GL_Texture gpu_copy=glBlitFramebuffer rgba_drm_format=AB24 rgba_rga_format=RK_FORMAT_RGBA_8888 nv12_rga_format=RK_FORMAT_YCbCr_420_SP modifier=not-specified vertical_flip=disabled");
		print_line("OPENVT_DMABUF_PIPELINE mode=two_stage rgba_slots=3 nv12_buffers=1 media_workers=1 gl_finish=enabled backpressure=blocking");
		print_line("OPENVT_ENCODED_SINK_CONFIG codec=h264 callback=synchronous storage=borrowed");
		return OK;
	}

	Error reserve_slot(int &r_slot_index, String &r_error) {
		if (!initialized) {
			r_error = "DMA-BUF pipeline is not initialized";
			return ERR_UNCONFIGURED;
		}

		{
			MutexLock lock(queue_mutex);
			if (worker_error != OK) {
				r_error = worker_error_message;
				return worker_error;
			}
			if (!accepting_frames) {
				r_error = "DMA-BUF pipeline is no longer accepting frames";
				return ERR_UNAVAILABLE;
			}
		}

		uint64_t wait_start = OS::get_singleton()->get_ticks_usec();
		bool waited = false;
		if (!free_slots_available.try_wait()) {
			waited = true;
			free_slots_available.wait();
		}
		uint64_t wait_end = OS::get_singleton()->get_ticks_usec();
		const uint64_t wait_duration = wait_end - wait_start;
		append_sample(producer_wait_samples, wait_duration);
		producer_wait_timer.observe(std::chrono::microseconds(wait_duration));
		if (waited) {
			backpressure_count++;
		}

		MutexLock lock(queue_mutex);
		if (worker_error != OK) {
			r_error = worker_error_message;
			return worker_error;
		}
		if (free_slot_indices.empty()) {
			r_error = "free slot semaphore and queue are inconsistent";
			return FAILED;
		}
		r_slot_index = free_slot_indices.front();
		free_slot_indices.pop_front();
		FrameSlot *slot = slots[r_slot_index];
		if (slot->state != FrameSlot::State::FREE) {
			r_error = "reserved DMA-BUF slot was not free";
			return FAILED;
		}
		slot->state = FrameSlot::State::RESERVED;
		return OK;
	}

	Error capture(GLuint p_source_texture, int p_slot_index, int p_frame_index, bool p_end_of_stream,
			String &r_error) {
		if (!initialized) {
			r_error = "DMA-BUF pipeline is not initialized";
			return ERR_UNCONFIGURED;
		}
		if (p_slot_index < 0 || p_slot_index >= FRAME_SLOT_COUNT) {
			r_error = "DMA-BUF slot index is invalid";
			return ERR_INVALID_PARAMETER;
		}
		if (p_source_texture == 0) {
			release_reserved_slot(p_slot_index);
			r_error = "Godot SubViewport texture native handle is zero";
			return ERR_INVALID_DATA;
		}

		FrameSlot *slot = slots[p_slot_index];

		uint64_t copy_start = OS::get_singleton()->get_ticks_usec();
		GLint previous_read_framebuffer = 0;
		GLint previous_draw_framebuffer = 0;
		GLint previous_texture = 0;
		glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read_framebuffer);
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_draw_framebuffer);
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);

		glBindFramebuffer(GL_READ_FRAMEBUFFER, source_framebuffer);
		glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, p_source_texture, 0);
		GLenum source_status = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
		if (source_status != GL_FRAMEBUFFER_COMPLETE) {
			restore_gl_state(previous_read_framebuffer, previous_draw_framebuffer, previous_texture);
			release_reserved_slot(p_slot_index);
			r_error = "Godot source framebuffer is incomplete: " + itos(static_cast<int64_t>(source_status));
			return ERR_INVALID_DATA;
		}

		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, slot->get_framebuffer());
		glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		glFinish();
		GLenum gl_error = glGetError();
		restore_gl_state(previous_read_framebuffer, previous_draw_framebuffer, previous_texture);
		if (gl_error != GL_NO_ERROR) {
			release_reserved_slot(p_slot_index);
			r_error = "GPU copy into DMA-BUF failed: " + itos(static_cast<int64_t>(gl_error));
			return FAILED;
		}
		uint64_t copy_end = OS::get_singleton()->get_ticks_usec();

		int ready_depth = 0;
		{
			MutexLock lock(queue_mutex);
			if (worker_error != OK) {
				r_error = worker_error_message;
				release_slot_locked(p_slot_index);
				free_slots_available.post();
				return worker_error;
			}
			slot->frame_index = p_frame_index;
			slot->end_of_stream = p_end_of_stream;
			slot->ready_usec = copy_end;
			slot->ready_ns = monotonic_now_ns();
			slot->state = FrameSlot::State::READY;
			ready_slot_indices.push_back(p_slot_index);
			ready_depth = static_cast<int>(ready_slot_indices.size());
			if (ready_depth > max_ready_queue_depth) {
				max_ready_queue_depth = ready_depth;
			}
			submitted_frames++;
			if (p_end_of_stream) {
				accepting_frames = false;
			}
		}
		const uint64_t copy_duration = copy_end - copy_start;
		append_sample(gpu_copy_samples, copy_duration);
		gpu_copy_timer.observe(std::chrono::microseconds(copy_duration));
		ready_slots_available.post();
		if (submitted_frames == 1 || submitted_frames % fps == 0) {
			print_line(vformat("OPENVT_DMABUF_SUBMIT frame=%d slot=%d source_texture=%d gpu_copy_ms=%.3f ready_depth=%d",
					submitted_frames, p_slot_index, static_cast<uint64_t>(p_source_texture),
					static_cast<double>(copy_end - copy_start) / 1000.0, ready_depth));
		}
		return OK;
	}

	void cancel_reserved_slot(int p_slot_index) {
		release_reserved_slot(p_slot_index);
	}

	Error finish(String &r_error) {
		if (!initialized || finished) {
			return OK;
		}
		{
			MutexLock lock(queue_mutex);
			accepting_frames = false;
			stop_requested = true;
		}
		ready_slots_available.post();
		if (media_thread.is_started()) {
			media_thread.wait_to_finish();
		}
		finished = true;

		MutexLock lock(queue_mutex);
		if (worker_error != OK) {
			r_error = worker_error_message;
			return worker_error;
		}
		if (encoded_frames != submitted_frames) {
			r_error = vformat("encoded frame count mismatch: submitted=%d encoded=%d", submitted_frames,
					encoded_frames);
			return FAILED;
		}
		if (total_frames > 0 && submitted_frames != total_frames) {
			r_error = vformat("submitted frame count mismatch: expected=%d actual=%d", total_frames,
					submitted_frames);
			return FAILED;
		}
		if (total_frames > 0 && !end_of_stream_encoded) {
			r_error = "MPP end-of-stream frame was not encoded";
			return FAILED;
		}
		return OK;
	}

	void shutdown() {
		if (media_thread.is_started()) {
			{
				MutexLock lock(queue_mutex);
				accepting_frames = false;
				stop_requested = true;
			}
			ready_slots_available.post();
			media_thread.wait_to_finish();
		}
		print_results();
		mpp_encoder.shutdown();
		if (source_framebuffer != 0) {
			glDeleteFramebuffers(1, &source_framebuffer);
			source_framebuffer = 0;
		}
		for (int i = 0; i < FRAME_SLOT_COUNT; i++) {
			if (slots[i] != nullptr) {
				memdelete(slots[i]);
				slots[i] = nullptr;
			}
		}
		initialized = false;
	}

private:
	static void media_thread_entry(void *p_userdata) {
		OpenVTDmaBufPipeline *pipeline = static_cast<OpenVTDmaBufPipeline *>(p_userdata);
		pipeline->media_thread_main();
	}

	void media_thread_main() {
		Thread::set_name("OpenVT RGA MPP");
		while (true) {
			ready_slots_available.wait();
			int slot_index = -1;
			{
				MutexLock lock(queue_mutex);
				if (!ready_slot_indices.empty()) {
					slot_index = ready_slot_indices.front();
					ready_slot_indices.pop_front();
					slots[slot_index]->state = FrameSlot::State::CONSUMING;
				} else if (stop_requested) {
					break;
				}
			}
			if (slot_index < 0) {
				continue;
			}

			FrameSlot *slot = slots[slot_index];
			uint64_t media_start = OS::get_singleton()->get_ticks_usec();
			uint64_t queue_wait = media_start - slot->ready_usec;
			String error_message;
			Error error = slot->get_rga_converter().convert(error_message);
			uint64_t rga_end = OS::get_singleton()->get_ticks_usec();
			MppPacketLease encoded_packet;
			uint64_t packet_pts_ns = 0;
			uint64_t packet_dts_ns = 0;
			bool keyframe = false;
			if (error == OK) {
				error = mpp_encoder.encode_frame(slot->ready_ns, slot->end_of_stream, encoded_packet,
						packet_pts_ns, packet_dts_ns, keyframe, error_message);
			}
			uint64_t mpp_end = OS::get_singleton()->get_ticks_usec();
			if (error == OK) {
				const uint64_t duration_ns = 1'000'000'000ULL / static_cast<uint64_t>(fps);
				vtb::EncodedVideoPacketView packet;
				packet.data = encoded_packet.data();
				packet.size = encoded_packet.size();
				packet.pts_ns = packet_pts_ns;
				packet.dts_ns = packet_dts_ns;
				packet.duration_ns = duration_ns;
				packet.sequence = static_cast<uint64_t>(slot->frame_index + 1);
				if (keyframe) {
					packet.keyframe = 1;
				}
				if (output_sink.publish(output_sink.context, &packet) != 0) {
					error = FAILED;
					error_message = "host rejected encoded video packet";
				}
			}
			uint64_t publish_end = OS::get_singleton()->get_ticks_usec();

			append_sample(queue_wait_samples, queue_wait);
			queue_wait_timer.observe(std::chrono::microseconds(queue_wait));
			const uint64_t rga_duration = rga_end - media_start;
			append_sample(rga_samples, rga_duration);
			rga_timer.observe(std::chrono::microseconds(rga_duration));
			if (error == OK) {
				const uint64_t mpp_duration = mpp_end - rga_end;
				const uint64_t publish_duration = publish_end - mpp_end;
				const uint64_t media_duration = publish_end - media_start;
				append_sample(mpp_samples, mpp_duration);
				append_sample(publish_samples, publish_duration);
				append_sample(media_samples, media_duration);
				mpp_timer.observe(std::chrono::microseconds(mpp_duration));
				publish_timer.observe(std::chrono::microseconds(publish_duration));
				media_timer.observe(std::chrono::microseconds(media_duration));
				encoded_frames++;
				if (slot->end_of_stream) {
					end_of_stream_encoded = true;
				}
				release_consumed_slot(slot_index);
				continue;
			}

			set_worker_error(error, error_message, slot_index);
			break;
		}
	}

	void set_worker_error(Error p_error, const String &p_message, int p_current_slot) {
		int released_slots = 0;
		{
			MutexLock lock(queue_mutex);
			if (worker_error == OK) {
				worker_error = p_error;
				worker_error_message = p_message;
			}
			accepting_frames = false;
			stop_requested = true;
			release_slot_locked(p_current_slot);
			released_slots++;
			while (!ready_slot_indices.empty()) {
				int slot_index = ready_slot_indices.front();
				ready_slot_indices.pop_front();
				release_slot_locked(slot_index);
				released_slots++;
			}
		}
		free_slots_available.post(released_slots + FRAME_SLOT_COUNT);
	}

	void release_consumed_slot(int p_slot_index) {
		{
			MutexLock lock(queue_mutex);
			release_slot_locked(p_slot_index);
		}
		free_slots_available.post();
	}

	void release_reserved_slot(int p_slot_index) {
		if (p_slot_index < 0 || p_slot_index >= FRAME_SLOT_COUNT) {
			return;
		}
		{
			MutexLock lock(queue_mutex);
			FrameSlot *slot = slots[p_slot_index];
			if (slot == nullptr || slot->state != FrameSlot::State::RESERVED) {
				return;
			}
			release_slot_locked(p_slot_index);
		}
		free_slots_available.post();
	}

	void release_slot_locked(int p_slot_index) {
		FrameSlot *slot = slots[p_slot_index];
		slot->state = FrameSlot::State::FREE;
		slot->frame_index = -1;
		slot->end_of_stream = false;
		slot->ready_usec = 0;
		slot->ready_ns = 0;
		free_slot_indices.push_back(p_slot_index);
	}

	void restore_gl_state(GLint p_read_framebuffer, GLint p_draw_framebuffer, GLint p_texture) {
		glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(p_read_framebuffer));
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(p_draw_framebuffer));
		glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(p_texture));
	}

	static void append_sample(std::deque<uint64_t> &p_samples, uint64_t p_sample) {
		if (p_samples.size() >= METRIC_WINDOW_CAPACITY) {
			p_samples.pop_front();
		}
		p_samples.push_back(p_sample);
	}

	static double percentile_ms(const std::deque<uint64_t> &p_samples, size_t p_percentile) {
		if (p_samples.empty()) {
			return 0.0;
		}
		std::vector<uint64_t> sorted(p_samples.begin(), p_samples.end());
		std::sort(sorted.begin(), sorted.end());
		size_t index = (sorted.size() * p_percentile + 99) / 100;
		if (index > 0) {
			index--;
		}
		if (index >= sorted.size()) {
			index = sorted.size() - 1;
		}
		return static_cast<double>(sorted[index]) / 1000.0;
	}

	static double duration_ms(std::chrono::nanoseconds p_duration) {
		return std::chrono::duration<double, std::milli>(p_duration).count();
	}

	void print_stage_metrics(const String &p_stage, const vtb::instrumentation::TimerHandle &p_timer,
			const std::deque<uint64_t> &p_samples) const {
		const vtb::instrumentation::TimerSnapshot snapshot = p_timer.snapshot();
		print_line(vformat("OPENVT_DMABUF_STAGE stage=%s samples=%d window_samples=%d latest_ms=%.3f min_ms=%.3f avg_ms=%.3f p50_ms=%.3f p95_ms=%.3f p99_ms=%.3f max_ms=%.3f",
				p_stage, snapshot.sample_count(), static_cast<uint64_t>(p_samples.size()),
				duration_ms(snapshot.latest()),
				duration_ms(snapshot.minimum()), duration_ms(snapshot.average()),
				percentile_ms(p_samples, 50), percentile_ms(p_samples, 95), percentile_ms(p_samples, 99),
				duration_ms(snapshot.maximum())));
	}

	void print_timer_metrics(const String &p_stage,
			const vtb::instrumentation::TimerHandle &p_timer) const {
		const vtb::instrumentation::TimerSnapshot snapshot = p_timer.snapshot();
		print_line(vformat("OPENVT_DMABUF_TIMER stage=%s samples=%d latest_ms=%.3f min_ms=%.3f avg_ms=%.3f max_ms=%.3f",
				p_stage, snapshot.sample_count(), duration_ms(snapshot.latest()),
				duration_ms(snapshot.minimum()), duration_ms(snapshot.average()),
				duration_ms(snapshot.maximum())));
	}

	void print_results() {
		if (results_printed || start_usec == 0) {
			return;
		}
		results_printed = true;
		uint64_t end_usec = OS::get_singleton()->get_ticks_usec();
		double elapsed_ms = static_cast<double>(end_usec - start_usec) / 1000.0;
		double throughput_fps = 0.0;
		if (elapsed_ms > 0.0) {
			throughput_fps = static_cast<double>(encoded_frames) * 1000.0 / elapsed_ms;
		}
		print_line(vformat("OPENVT_DMABUF_RESULT submitted=%d encoded=%d total_ms=%.3f throughput_fps=%.3f backpressure_count=%d max_ready_depth=%d encoded_bytes=%d",
				submitted_frames, encoded_frames, elapsed_ms, throughput_fps, backpressure_count,
				max_ready_queue_depth, static_cast<uint64_t>(mpp_encoder.get_encoded_bytes())));
		print_stage_metrics("producer_wait", producer_wait_timer, producer_wait_samples);
		print_stage_metrics("gpu_copy", gpu_copy_timer, gpu_copy_samples);
		print_stage_metrics("queue_wait", queue_wait_timer, queue_wait_samples);
		print_stage_metrics("rga", rga_timer, rga_samples);
		print_stage_metrics("mpp", mpp_timer, mpp_samples);
		print_stage_metrics("output_publish_call", publish_timer, publish_samples);
		print_stage_metrics("media", media_timer, media_samples);

	}

	int width = 0;
	int height = 0;
	int fps = 0;
	int total_frames = 0;
	int rgba_stride = 0;
	int nv12_stride = 0;
	int nv12_vertical_stride = 0;
	bool initialized = false;
	bool finished = false;
	bool accepting_frames = false;
	bool stop_requested = false;
	bool end_of_stream_encoded = false;
	bool results_printed = false;
	static constexpr size_t METRIC_WINDOW_CAPACITY = 4096;
	GLuint source_framebuffer = 0;
	vtb::instrumentation::TimerRegistry timers;
	vtb::instrumentation::TimerHandle producer_wait_timer;
	vtb::instrumentation::TimerHandle gpu_copy_timer;
	vtb::instrumentation::TimerHandle queue_wait_timer;
	vtb::instrumentation::TimerHandle rga_timer;
	vtb::instrumentation::TimerHandle mpp_timer;
	vtb::instrumentation::TimerHandle publish_timer;
	vtb::instrumentation::TimerHandle media_timer;
	DmaBuffer nv12_buffer;
	FrameSlot *slots[FRAME_SLOT_COUNT] = {};
	MppEncoder mpp_encoder;
	vtb::EncodedVideoSink output_sink;
	Thread media_thread;
	BinaryMutex queue_mutex;
	Semaphore free_slots_available;
	Semaphore ready_slots_available;
	std::deque<int> free_slot_indices;
	std::deque<int> ready_slot_indices;
	Error worker_error = OK;
	String worker_error_message;
	uint64_t start_usec = 0;
	int submitted_frames = 0;
	int encoded_frames = 0;
	int backpressure_count = 0;
	int max_ready_queue_depth = 0;
	std::deque<uint64_t> producer_wait_samples;
	std::deque<uint64_t> gpu_copy_samples;
	std::deque<uint64_t> queue_wait_samples;
	std::deque<uint64_t> rga_samples;
	std::deque<uint64_t> mpp_samples;
	std::deque<uint64_t> publish_samples;
	std::deque<uint64_t> media_samples;
};

OpenVTDmaBufCapture::~OpenVTDmaBufCapture() {
	if (pipeline != nullptr) {
		finish();
	}
}

void OpenVTDmaBufCapture::_bind_methods() {
	ClassDB::bind_method(D_METHOD("capture_frame", "viewport", "frame_index", "end_of_stream"),
			&OpenVTDmaBufCapture::capture_frame);
	ClassDB::bind_method(D_METHOD("finish"), &OpenVTDmaBufCapture::finish);
	ClassDB::bind_method(D_METHOD("get_last_error"), &OpenVTDmaBufCapture::get_last_error);
}

Error OpenVTDmaBufCapture::configure(const vtb::EncodedVideoSink &p_sink, int p_width, int p_height, int p_fps,
		int p_total_frames) {
	if (configured || pipeline != nullptr) {
		operation_error_message = "capture is already configured";
		return ERR_ALREADY_IN_USE;
	}
	if (p_sink.context == nullptr || p_sink.configure_h264 == nullptr || p_sink.publish == nullptr || p_width <= 0 || p_height <= 0 || p_fps <= 0 || p_total_frames < 0) {
		operation_error_message = "invalid DMA-BUF capture configuration";
		return ERR_INVALID_PARAMETER;
	}
	if (p_width % 2 != 0 || p_height % 2 != 0) {
		operation_error_message = "NV12 output requires even width and height";
		return ERR_INVALID_PARAMETER;
	}

	output_sink = p_sink;
	width = p_width;
	height = p_height;
	fps = p_fps;
	total_frames = p_total_frames;
	configured = true;
	finished = false;
	operation_error_message = String();
	return OK;
}

Error OpenVTDmaBufCapture::capture_frame(RID p_viewport, int p_frame_index, bool p_end_of_stream) {
	if (!configured || finished) {
		operation_error_message = "capture is not configured or is already finished";
		return ERR_UNCONFIGURED;
	}
	if (!p_viewport.is_valid()) {
		operation_error_message = "SubViewport RID is invalid";
		return ERR_INVALID_PARAMETER;
	}

	if (pipeline == nullptr) {
		operation_error = OK;
		operation_error_message = String();
		RenderingServer::get_singleton()->call_on_render_thread(
				callable_mp(this, &OpenVTDmaBufCapture::_initialize_pipeline_render_thread));
		operation_finished.wait();
		if (operation_error != OK) {
			return operation_error;
		}
	}

	int slot_index = -1;
	String reserve_error_message;
	Error reserve_error = pipeline->reserve_slot(slot_index, reserve_error_message);
	if (reserve_error != OK) {
		operation_error_message = reserve_error_message;
		return reserve_error;
	}

	operation_error = OK;
	operation_error_message = String();
	RenderingServer::get_singleton()->call_on_render_thread(
			callable_mp(this, &OpenVTDmaBufCapture::_capture_frame_render_thread)
					.bind(p_viewport, slot_index, p_frame_index, p_end_of_stream));
	operation_finished.wait();
	return operation_error;
}

Error OpenVTDmaBufCapture::finish() {
	if (finished) {
		return OK;
	}
	if (!configured && pipeline == nullptr) {
		finished = true;
		return OK;
	}

	operation_error = OK;
	operation_error_message = String();
	RenderingServer::get_singleton()->call_on_render_thread(
			callable_mp(this, &OpenVTDmaBufCapture::_finish_render_thread));
	operation_finished.wait();
	finished = true;
	return operation_error;
}

String OpenVTDmaBufCapture::get_last_error() const {
	return operation_error_message;
}

void OpenVTDmaBufCapture::_initialize_pipeline_render_thread() {
	String error_message;
	if (pipeline != nullptr) {
		_complete_operation(OK, String());
		return;
	}
	pipeline = memnew(OpenVTDmaBufPipeline);
	Error init_error = pipeline->initialize(output_sink, width, height, fps, total_frames, error_message);
	if (init_error != OK) {
		memdelete(pipeline);
		pipeline = nullptr;
		_complete_operation(init_error, error_message);
		return;
	}
	_complete_operation(OK, String());
}

void OpenVTDmaBufCapture::_capture_frame_render_thread(RID p_viewport, int p_slot_index, int p_frame_index,
		bool p_end_of_stream) {
	String error_message;
	RID texture = RenderingServer::get_singleton()->viewport_get_texture(p_viewport);
	if (!texture.is_valid()) {
		pipeline->cancel_reserved_slot(p_slot_index);
		_complete_operation(ERR_INVALID_DATA, "Godot SubViewport render-target texture RID is invalid");
		return;
	}
	uint64_t native_handle = RenderingServer::get_singleton()->texture_get_native_handle(texture, false);
	Error capture_error = pipeline->capture(static_cast<GLuint>(native_handle), p_slot_index, p_frame_index,
			p_end_of_stream, error_message);
	_complete_operation(capture_error, error_message);
}

void OpenVTDmaBufCapture::_finish_render_thread() {
	Error finish_error = OK;
	String error_message;
	if (pipeline != nullptr) {
		finish_error = pipeline->finish(error_message);
		memdelete(pipeline);
		pipeline = nullptr;
	}
	_complete_operation(finish_error, error_message);
}

void OpenVTDmaBufCapture::_complete_operation(Error p_error, const String &p_message) {
	operation_error = p_error;
	operation_error_message = p_message;
	if (p_error != OK) {
		ERR_PRINT("OPENVT_DMABUF_ERROR " + p_message);
	}
	operation_finished.post();
}

