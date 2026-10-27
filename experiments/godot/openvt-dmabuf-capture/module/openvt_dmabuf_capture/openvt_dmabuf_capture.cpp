#define MODULE_TAG "openvt_dmabuf_capture"

#include "openvt_dmabuf_capture.h"

#include "core/io/file_access.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/memory.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "platform/linuxbsd/platform_egl.h"
#include "platform/linuxbsd/platform_gl.h"
#include "servers/rendering/rendering_server.h"

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
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

constexpr const char *DMA_HEAP_PATH = "/dev/dma_heap/system-uncached";
constexpr EGLenum EGL_LINUX_DMA_BUF_TARGET = 0x3270;
constexpr EGLint EGL_LINUX_DRM_FOURCC_ATTRIBUTE = 0x3271;
constexpr EGLint EGL_DMA_BUF_PLANE0_FD_ATTRIBUTE = 0x3272;
constexpr EGLint EGL_DMA_BUF_PLANE0_OFFSET_ATTRIBUTE = 0x3273;
constexpr EGLint EGL_DMA_BUF_PLANE0_PITCH_ATTRIBUTE = 0x3274;

using EglCreateImageKhrProc = EGLImageKHR (*)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
using EglDestroyImageKhrProc = EGLBoolean (*)(EGLDisplay, EGLImageKHR);
using GlEglImageTargetTexture2DProc = void (*)(GLenum, void *);

String errno_message(const String &p_prefix) {
	return p_prefix + ": " + String::utf8(std::strerror(errno));
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

class MppEncoder {
public:
	MppEncoder() = default;
	~MppEncoder() { shutdown(); }

	MppEncoder(const MppEncoder &) = delete;
	MppEncoder &operator=(const MppEncoder &) = delete;
	MppEncoder(MppEncoder &&) = delete;
	MppEncoder &operator=(MppEncoder &&) = delete;

	Error initialize(const DmaBuffer &p_input, const String &p_output_path, int p_width, int p_height,
			int p_stride, int p_vertical_stride, int p_fps, String &r_error) {
		output = FileAccess::open(p_output_path, FileAccess::WRITE);
		if (output.is_null()) {
			r_error = "open output file failed: " + p_output_path;
			return FileAccess::get_open_error();
		}

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
			"rc:fps_out_num", "rc:fps_out_denorm", "rc:gop", "rc:bps_target", "rc:bps_max",
			"rc:bps_min", "codec:type", "h264:profile", "h264:level", "h264:cabac_en",
			"h264:cabac_idc", "h264:trans8x8",
		};
		RK_S32 config_values[] = {
			p_width, p_height, p_stride, p_vertical_stride, MPP_FMT_YUV420SP,
			MPP_ENC_RC_MODE_CBR, 0, p_fps, 1, 0,
			p_fps, 1, p_fps * 2, 8'000'000, 8'500'000,
			7'500'000, MPP_VIDEO_CodingAVC, 100, 40, 1,
			0, 1,
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
		fps = p_fps;
		return OK;
	}

	Error encode_frame(int p_frame_index, bool p_end_of_stream, String &r_error) {
		mpp_frame_set_pts(frame, static_cast<RK_S64>(p_frame_index) * 1'000'000 / fps);
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

		void *position = mpp_packet_get_pos(packet);
		size_t length = mpp_packet_get_length(packet);
		if (position == nullptr || length == 0) {
			mpp_packet_deinit(&packet);
			r_error = "MPP returned an empty packet";
			return ERR_INVALID_DATA;
		}

		bool stored = output->store_buffer(static_cast<const uint8_t *>(position), length);
		mpp_packet_deinit(&packet);
		if (!stored) {
			r_error = "write encoded packet failed";
			return ERR_FILE_CANT_WRITE;
		}

		encoded_bytes += length;
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
		if (output.is_valid()) {
			output->close();
			output.unref();
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

	MppCtx context = nullptr;
	MppApi *api = nullptr;
	MppEncCfg config = nullptr;
	MppBuffer input_buffer = nullptr;
	MppFrame frame = nullptr;
	Ref<FileAccess> output;
	int fps = 0;
	size_t encoded_bytes = 0;
};

} // namespace

class OpenVTDmaBufPipeline {
public:
	OpenVTDmaBufPipeline() = default;
	~OpenVTDmaBufPipeline() { shutdown(); }

	OpenVTDmaBufPipeline(const OpenVTDmaBufPipeline &) = delete;
	OpenVTDmaBufPipeline &operator=(const OpenVTDmaBufPipeline &) = delete;
	OpenVTDmaBufPipeline(OpenVTDmaBufPipeline &&) = delete;
	OpenVTDmaBufPipeline &operator=(OpenVTDmaBufPipeline &&) = delete;

	Error initialize(const String &p_output_path, int p_width, int p_height, int p_fps, int p_total_frames,
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

		Error error = rgba_buffer.allocate(DMA_HEAP_PATH, rgba_size, r_error);
		if (error != OK) {
			return error;
		}
		error = nv12_buffer.allocate(DMA_HEAP_PATH, nv12_size, r_error);
		if (error != OK) {
			return error;
		}
		error = dma_target.initialize(rgba_buffer, width, height, rgba_stride, r_error);
		if (error != OK) {
			return error;
		}

		glGenFramebuffers(1, &source_framebuffer);
		if (source_framebuffer == 0) {
			r_error = "failed to allocate source framebuffer";
			return ERR_CANT_CREATE;
		}

		error = rga_converter.initialize(rgba_buffer, nv12_buffer, width, height, rgba_stride, nv12_stride,
				nv12_vertical_stride, r_error);
		if (error != OK) {
			return error;
		}
		error = mpp_encoder.initialize(nv12_buffer, p_output_path, width, height, nv12_stride,
				nv12_vertical_stride, fps, r_error);
		if (error != OK) {
			return error;
		}

		start_usec = OS::get_singleton()->get_ticks_usec();
		print_line(vformat("OPENVT_DMABUF_CONFIG width=%d height=%d fps=%d frames=%d rgba_fd=%d nv12_fd=%d rgba_stride=%d nv12_stride=%d nv12_vertical_stride=%d heap=%s",
				width, height, fps, total_frames, rgba_buffer.get_fd(), nv12_buffer.get_fd(), rgba_stride,
				nv12_stride, nv12_vertical_stride, DMA_HEAP_PATH));
		print_line("OPENVT_DMABUF_LAYOUT source=Godot_SubViewport_GL_Texture gpu_copy=glBlitFramebuffer rgba_drm_format=AB24 rgba_rga_format=RK_FORMAT_RGBA_8888 nv12_rga_format=RK_FORMAT_YCbCr_420_SP modifier=not-specified vertical_flip=disabled");
		initialized = true;
		return OK;
	}

	Error capture(GLuint p_source_texture, int p_frame_index, bool p_end_of_stream, String &r_error) {
		if (!initialized) {
			r_error = "DMA-BUF pipeline is not initialized";
			return ERR_UNCONFIGURED;
		}
		if (p_source_texture == 0) {
			r_error = "Godot SubViewport texture native handle is zero";
			return ERR_INVALID_DATA;
		}

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
			r_error = "Godot source framebuffer is incomplete: " + itos(static_cast<int64_t>(source_status));
			return ERR_INVALID_DATA;
		}

		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dma_target.get_framebuffer());
		glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		glFinish();
		GLenum gl_error = glGetError();
		restore_gl_state(previous_read_framebuffer, previous_draw_framebuffer, previous_texture);
		if (gl_error != GL_NO_ERROR) {
			r_error = "GPU copy into DMA-BUF failed: " + itos(static_cast<int64_t>(gl_error));
			return FAILED;
		}
		uint64_t copy_end = OS::get_singleton()->get_ticks_usec();

		Error error = rga_converter.convert(r_error);
		if (error != OK) {
			return error;
		}
		uint64_t rga_end = OS::get_singleton()->get_ticks_usec();
		error = mpp_encoder.encode_frame(p_frame_index, p_end_of_stream, r_error);
		if (error != OK) {
			return error;
		}
		uint64_t mpp_end = OS::get_singleton()->get_ticks_usec();

		gpu_copy_total_usec += copy_end - copy_start;
		rga_total_usec += rga_end - copy_end;
		mpp_total_usec += mpp_end - rga_end;
		captured_frames++;
		if (captured_frames == 1 || captured_frames % fps == 0) {
			print_line(vformat("OPENVT_DMABUF_FRAME frame=%d source_texture=%d gpu_copy_ms=%.3f rga_ms=%.3f mpp_ms=%.3f",
					captured_frames, static_cast<uint64_t>(p_source_texture),
					static_cast<double>(copy_end - copy_start) / 1000.0,
					static_cast<double>(rga_end - copy_end) / 1000.0,
					static_cast<double>(mpp_end - rga_end) / 1000.0));
		}
		return OK;
	}

	void shutdown() {
		if (!initialized && source_framebuffer == 0) {
			return;
		}

		uint64_t end_usec = OS::get_singleton()->get_ticks_usec();
		if (captured_frames > 0) {
			double elapsed_ms = static_cast<double>(end_usec - start_usec) / 1000.0;
			print_line(vformat("OPENVT_DMABUF_RESULT frames=%d total_ms=%.3f throughput_fps=%.3f gpu_copy_avg_ms=%.3f rga_avg_ms=%.3f mpp_avg_ms=%.3f encoded_bytes=%d",
					captured_frames, elapsed_ms, static_cast<double>(captured_frames) * 1000.0 / elapsed_ms,
					static_cast<double>(gpu_copy_total_usec) / captured_frames / 1000.0,
					static_cast<double>(rga_total_usec) / captured_frames / 1000.0,
					static_cast<double>(mpp_total_usec) / captured_frames / 1000.0,
					static_cast<uint64_t>(mpp_encoder.get_encoded_bytes())));
		}

		mpp_encoder.shutdown();
		rga_converter.shutdown();
		if (source_framebuffer != 0) {
			glDeleteFramebuffers(1, &source_framebuffer);
			source_framebuffer = 0;
		}
		dma_target.shutdown();
		initialized = false;
	}

private:
	void restore_gl_state(GLint p_read_framebuffer, GLint p_draw_framebuffer, GLint p_texture) {
		glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(p_read_framebuffer));
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(p_draw_framebuffer));
		glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(p_texture));
	}

	int width = 0;
	int height = 0;
	int fps = 0;
	int total_frames = 0;
	int rgba_stride = 0;
	int nv12_stride = 0;
	int nv12_vertical_stride = 0;
	bool initialized = false;
	GLuint source_framebuffer = 0;
	DmaBuffer rgba_buffer;
	DmaBuffer nv12_buffer;
	ExternalDmaBufTarget dma_target;
	RgaConverter rga_converter;
	MppEncoder mpp_encoder;
	uint64_t start_usec = 0;
	uint64_t gpu_copy_total_usec = 0;
	uint64_t rga_total_usec = 0;
	uint64_t mpp_total_usec = 0;
	int captured_frames = 0;
};

OpenVTDmaBufCapture::~OpenVTDmaBufCapture() {
	if (pipeline != nullptr) {
		finish();
	}
}

void OpenVTDmaBufCapture::_bind_methods() {
	ClassDB::bind_method(D_METHOD("configure", "output_path", "width", "height", "fps", "total_frames"),
			&OpenVTDmaBufCapture::configure);
	ClassDB::bind_method(D_METHOD("capture_frame", "viewport", "frame_index", "end_of_stream"),
			&OpenVTDmaBufCapture::capture_frame);
	ClassDB::bind_method(D_METHOD("finish"), &OpenVTDmaBufCapture::finish);
	ClassDB::bind_method(D_METHOD("get_last_error"), &OpenVTDmaBufCapture::get_last_error);
}

Error OpenVTDmaBufCapture::configure(const String &p_output_path, int p_width, int p_height, int p_fps,
		int p_total_frames) {
	if (configured || pipeline != nullptr) {
		operation_error_message = "capture is already configured";
		return ERR_ALREADY_IN_USE;
	}
	if (p_output_path.is_empty() || p_width <= 0 || p_height <= 0 || p_fps <= 0 || p_total_frames <= 0) {
		operation_error_message = "invalid DMA-BUF capture configuration";
		return ERR_INVALID_PARAMETER;
	}
	if (p_width % 2 != 0 || p_height % 2 != 0) {
		operation_error_message = "NV12 output requires even width and height";
		return ERR_INVALID_PARAMETER;
	}

	output_path = p_output_path;
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

	operation_error = OK;
	operation_error_message = String();
	RenderingServer::get_singleton()->call_on_render_thread(
			callable_mp(this, &OpenVTDmaBufCapture::_capture_frame_render_thread)
					.bind(p_viewport, p_frame_index, p_end_of_stream));
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

void OpenVTDmaBufCapture::_capture_frame_render_thread(RID p_viewport, int p_frame_index, bool p_end_of_stream) {
	String error_message;
	if (pipeline == nullptr) {
		pipeline = memnew(OpenVTDmaBufPipeline);
		Error init_error = pipeline->initialize(output_path, width, height, fps, total_frames, error_message);
		if (init_error != OK) {
			memdelete(pipeline);
			pipeline = nullptr;
			_complete_operation(init_error, error_message);
			return;
		}
	}

	RID texture = RenderingServer::get_singleton()->viewport_get_texture(p_viewport);
	if (!texture.is_valid()) {
		_complete_operation(ERR_INVALID_DATA, "Godot SubViewport render-target texture RID is invalid");
		return;
	}
	uint64_t native_handle = RenderingServer::get_singleton()->texture_get_native_handle(texture, false);
	Error capture_error = pipeline->capture(static_cast<GLuint>(native_handle), p_frame_index,
			p_end_of_stream, error_message);
	_complete_operation(capture_error, error_message);
}

void OpenVTDmaBufCapture::_finish_render_thread() {
	if (pipeline != nullptr) {
		memdelete(pipeline);
		pipeline = nullptr;
	}
	_complete_operation(OK, String());
}

void OpenVTDmaBufCapture::_complete_operation(Error p_error, const String &p_message) {
	operation_error = p_error;
	operation_error_message = p_message;
	if (p_error != OK) {
		ERR_PRINT("OPENVT_DMABUF_ERROR " + p_message);
	}
	operation_finished.post();
}

