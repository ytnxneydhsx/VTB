#define MODULE_TAG "openvt_dmabuf_capture"

#include "openvt_dmabuf_capture.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/memory.h"
#include "core/string/print_string.h"
#include "platform/linuxbsd/platform_egl.h"
#include "platform/linuxbsd/platform_gl.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/rendering_server.h"

#include <drm/drm_fourcc.h>
#include <linux/dma-heap.h>

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

class UniqueFd final {
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

private:
	int release() {
		const int result = fd;
		fd = -1;
		return result;
	}

	void reset(int p_replacement = -1) {
		if (fd >= 0) {
			close(fd);
		}
		fd = p_replacement;
	}

	int fd = -1;
};

class DmaBuffer final {
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
		const CharString path = p_heap_path.utf8();
		UniqueFd heap_fd(open(path.get_data(), O_RDONLY | O_CLOEXEC));
		if (heap_fd.get() < 0) {
			r_error = errno_message("open DMA heap failed: " + p_heap_path);
			return ERR_CANT_OPEN;
		}
		dma_heap_allocation_data allocation{};
		allocation.len = p_size;
		allocation.fd_flags = O_RDWR | O_CLOEXEC;
		if (ioctl(heap_fd.get(), DMA_HEAP_IOCTL_ALLOC, &allocation) != 0) {
			r_error = errno_message("DMA_HEAP_IOCTL_ALLOC failed");
			return ERR_OUT_OF_MEMORY;
		}
		buffer_fd = UniqueFd(static_cast<int>(allocation.fd));
		buffer_size = p_size;
		return OK;
	}

	int get_fd() const { return buffer_fd.get(); }
	size_t get_size() const { return buffer_size; }

private:
	UniqueFd buffer_fd;
	size_t buffer_size = 0;
};

class ExternalDmaBufTarget final {
public:
	ExternalDmaBufTarget() = default;
	~ExternalDmaBufTarget() { shutdown(); }

	ExternalDmaBufTarget(const ExternalDmaBufTarget &) = delete;
	ExternalDmaBufTarget &operator=(const ExternalDmaBufTarget &) = delete;
	ExternalDmaBufTarget(ExternalDmaBufTarget &&) = delete;
	ExternalDmaBufTarget &operator=(ExternalDmaBufTarget &&) = delete;

	Error initialize(const DmaBuffer &p_buffer, int p_width, int p_height,
			int p_stride, String &r_error) {
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
		const EGLint attributes[] = {
			EGL_WIDTH, p_width,
			EGL_HEIGHT, p_height,
			EGL_LINUX_DRM_FOURCC_ATTRIBUTE, static_cast<EGLint>(DRM_FORMAT_ABGR8888),
			EGL_DMA_BUF_PLANE0_FD_ATTRIBUTE, p_buffer.get_fd(),
			EGL_DMA_BUF_PLANE0_OFFSET_ATTRIBUTE, 0,
			EGL_DMA_BUF_PLANE0_PITCH_ATTRIBUTE, p_stride,
			EGL_NONE,
		};
		image = create_image(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_TARGET, nullptr, attributes);
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
		GLint previous_framebuffer = 0;
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_framebuffer);
		GLuint validation_framebuffer = 0;
		glGenFramebuffers(1, &validation_framebuffer);
		glBindFramebuffer(GL_FRAMEBUFFER, validation_framebuffer);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
		const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous_framebuffer));
		glDeleteFramebuffers(1, &validation_framebuffer);
		if (status != GL_FRAMEBUFFER_COMPLETE) {
			r_error = "DMA-BUF output framebuffer is incomplete: " + itos(static_cast<int64_t>(status));
			return ERR_CANT_CREATE;
		}
		return OK;
	}

	void shutdown() {
		if (texture != 0) {
			glDeleteTextures(1, &texture);
			texture = 0;
		}
		if (image != nullptr && destroy_image != nullptr && display != EGL_NO_DISPLAY) {
			destroy_image(display, image);
			image = nullptr;
		}
	}

	GLuint get_texture() const { return texture; }

private:
	EGLDisplay display = EGL_NO_DISPLAY;
	EGLImageKHR image = nullptr;
	EglCreateImageKhrProc create_image = nullptr;
	EglDestroyImageKhrProc destroy_image = nullptr;
	GlEglImageTargetTexture2DProc image_target_texture = nullptr;
	GLuint texture = 0;
};

class FrameSlot final {
public:
	enum class State { FREE, READY };

	FrameSlot() = default;
	~FrameSlot() = default;
	FrameSlot(const FrameSlot &) = delete;
	FrameSlot &operator=(const FrameSlot &) = delete;
	FrameSlot(FrameSlot &&) = delete;
	FrameSlot &operator=(FrameSlot &&) = delete;

	Error initialize(int p_width, int p_height, int p_stride,
			size_t p_size, String &r_error) {
		Error error = rgba_buffer.allocate(DMA_HEAP_PATH, p_size, r_error);
		if (error != OK) {
			return error;
		}
		return target.initialize(rgba_buffer, p_width, p_height, p_stride, r_error);
	}

	int fd() const { return rgba_buffer.get_fd(); }
	size_t size() const { return rgba_buffer.get_size(); }
	GLuint texture() const { return target.get_texture(); }

	State state = State::FREE;
	int frame_index = -1;
	bool end_of_stream = false;

private:
	DmaBuffer rgba_buffer;
	ExternalDmaBufTarget target;
};

} // namespace

class OpenVTRgbaFramePipeline final {
public:
	OpenVTRgbaFramePipeline() = default;
	~OpenVTRgbaFramePipeline() { shutdown(); }
	OpenVTRgbaFramePipeline(const OpenVTRgbaFramePipeline &) = delete;
	OpenVTRgbaFramePipeline &operator=(const OpenVTRgbaFramePipeline &) = delete;
	OpenVTRgbaFramePipeline(OpenVTRgbaFramePipeline &&) = delete;
	OpenVTRgbaFramePipeline &operator=(OpenVTRgbaFramePipeline &&) = delete;

	Error initialize(const vtb::RgbaFrameSink &p_sink, RID p_viewport,
			int p_width, int p_height, int p_fps, int p_total_frames,
			String &r_error) {
		width = p_width;
		height = p_height;
		fps = p_fps;
		total_frames = p_total_frames;
		rgba_stride = width * 4;
		const size_t size = static_cast<size_t>(rgba_stride) * height;
		Error error = slot.initialize(width, height, rgba_stride, size, r_error);
		if (error != OK) {
			return error;
		}
		const RID render_target = RenderingServer::get_singleton()->viewport_get_render_target(p_viewport);
		if (!render_target.is_valid()) {
			r_error = "Godot viewport render target is invalid";
			return ERR_INVALID_DATA;
		}
		external_texture = RSG::texture_storage->texture_create_from_native_handle(
				RSE::TEXTURE_TYPE_2D, Image::FORMAT_RGBA8,
				static_cast<uint64_t>(slot.texture()), width, height, 1);
		if (!external_texture.is_valid()) {
			r_error = "failed to wrap RGBA DMA-BUF texture for Godot";
			return ERR_CANT_CREATE;
		}
		viewport_render_target = render_target;
		RSG::texture_storage->render_target_set_override(
				viewport_render_target, external_texture, RID(), RID(), RID());
		if (RSG::texture_storage->render_target_get_override_color(
					viewport_render_target) != external_texture) {
			r_error = "Godot rejected the RGBA DMA-BUF color attachment";
			return ERR_CANT_CREATE;
		}
		rgba_sink = p_sink;
		accepting_frames = true;
		initialized = true;
		print_line(vformat("OPENVT_RGBA_CONFIG width=%d height=%d fps=%d frames=%d rgba_fd=%d rgba_stride=%d heap=%s",
				width, height, fps, total_frames, slot.fd(), rgba_stride, DMA_HEAP_PATH));
		print_line("OPENVT_RGBA_PIPELINE steps=direct_viewport_render,wait_gpu_render,publish_rgba_frame boundary=rgba metrics=host");
		return OK;
	}

	Error wait_for_render(int p_frame_index, bool p_eos, String &r_error) {
		if (!initialized || pipeline_error != OK || !accepting_frames) {
			r_error = pipeline_error_message;
			if (r_error.is_empty()) {
				r_error = "RGBA pipeline is unavailable";
			}
			if (pipeline_error != OK) {
				return pipeline_error;
			}
			return ERR_UNAVAILABLE;
		}
		if (slot.state != FrameSlot::State::FREE) {
			r_error = "RGBA DMA-BUF is still borrowed by the previous frame";
			return ERR_BUSY;
		}
		slot.frame_index = p_frame_index;
		slot.end_of_stream = p_eos;
		glFinish();
		if (glGetError() != GL_NO_ERROR) {
			return fail(FAILED, "Godot render completion wait failed", r_error);
		}
		slot.state = FrameSlot::State::READY;
		++rendered_frames;
		if (p_eos) {
			accepting_frames = false;
		}
		return OK;
	}

	Error publish(uint64_t p_timestamp_ns, String &r_error) {
		if (slot.state != FrameSlot::State::READY || p_timestamp_ns == 0) {
			return fail(ERR_INVALID_PARAMETER, "RGBA frame is not ready", r_error);
		}
		vtb::RgbaFrameView frame;
		frame.dma_buf_fd = slot.fd();
		frame.dma_buf_size = slot.size();
		frame.buffer_id = 0;
		frame.stream_generation = 1;
		frame.width = static_cast<uint32_t>(width);
		frame.height = static_cast<uint32_t>(height);
		frame.horizontal_stride = static_cast<uint32_t>(rgba_stride / 4);
		frame.vertical_stride = static_cast<uint32_t>(height);
		frame.stride_bytes = static_cast<uint32_t>(rgba_stride);
		frame.frames_per_second = static_cast<uint32_t>(fps);
		frame.timestamp_ns = p_timestamp_ns;
		frame.sequence = static_cast<uint64_t>(slot.frame_index + 1);
		frame.format = vtb::RgbaFrameFormat::rgba8888;
		if (slot.end_of_stream) {
			frame.end_of_stream = 1;
		}
		if (rgba_sink.submit(rgba_sink.context, &frame) != 0) {
			return fail(FAILED, "host rejected RGBA DMA-BUF frame", r_error);
		}
		++delivered_frames;
		if (slot.end_of_stream) {
			end_of_stream_delivered = true;
		}
		release_slot();
		return OK;
	}

	Error finish(String &r_error) {
		if (!initialized || finished) {
			return OK;
		}
		accepting_frames = false;
		finished = true;
		if (pipeline_error != OK) {
			r_error = pipeline_error_message;
			return pipeline_error;
		}
		if (slot.state != FrameSlot::State::FREE || delivered_frames != rendered_frames) {
			r_error = "RGBA frame sequence ended before publication";
			return FAILED;
		}
		if (total_frames > 0 && delivered_frames != total_frames) {
			r_error = "RGBA delivered frame count does not match configuration";
			return FAILED;
		}
		if (total_frames > 0 && !end_of_stream_delivered) {
			r_error = "RGBA end-of-stream frame was not delivered";
			return FAILED;
		}
		return OK;
	}

	void shutdown() {
		accepting_frames = false;
		if (viewport_render_target.is_valid()) {
			RSG::texture_storage->render_target_set_override(
					viewport_render_target, RID(), RID(), RID(), RID());
			viewport_render_target = RID();
		}
		if (external_texture.is_valid()) {
			RSG::texture_storage->texture_free(external_texture);
			external_texture = RID();
		}
		initialized = false;
	}

private:
	Error fail(Error p_error, const String &p_message, String &r_error) {
		pipeline_error = p_error;
		pipeline_error_message = p_message;
		accepting_frames = false;
		release_slot();
		r_error = p_message;
		return p_error;
	}

	void release_slot() {
		slot.state = FrameSlot::State::FREE;
		slot.frame_index = -1;
		slot.end_of_stream = false;
	}

	int width = 0;
	int height = 0;
	int fps = 0;
	int total_frames = 0;
	int rgba_stride = 0;
	bool initialized = false;
	bool finished = false;
	bool accepting_frames = false;
	bool end_of_stream_delivered = false;
	RID viewport_render_target;
	RID external_texture;
	FrameSlot slot;
	vtb::RgbaFrameSink rgba_sink;
	Error pipeline_error = OK;
	String pipeline_error_message;
	int rendered_frames = 0;
	int delivered_frames = 0;
};

OpenVTDmaBufCapture::~OpenVTDmaBufCapture() {
	if (pipeline != nullptr) {
		finish();
	}
}

void OpenVTDmaBufCapture::_bind_methods() {
	ClassDB::bind_method(D_METHOD("wait_gpu_render", "frame_index", "end_of_stream"), &OpenVTDmaBufCapture::wait_gpu_render);
	ClassDB::bind_method(D_METHOD("publish_rgba_frame", "timestamp_ns"), &OpenVTDmaBufCapture::publish_rgba_frame);
	ClassDB::bind_method(D_METHOD("finish"), &OpenVTDmaBufCapture::finish);
	ClassDB::bind_method(D_METHOD("get_last_error"), &OpenVTDmaBufCapture::get_last_error);
}

Error OpenVTDmaBufCapture::configure(const vtb::RgbaFrameSink &p_sink,
		RID p_viewport, int p_width, int p_height, int p_fps, int p_total_frames) {
	if (configured || pipeline != nullptr) {
		operation_error_message = "capture is already configured";
		return ERR_ALREADY_IN_USE;
	}
	if (p_sink.context == nullptr || p_sink.submit == nullptr || !p_viewport.is_valid() ||
			p_width <= 0 || p_height <= 0 || p_fps <= 0 || p_total_frames < 0) {
		operation_error_message = "invalid RGBA capture configuration";
		return ERR_INVALID_PARAMETER;
	}
	rgba_sink = p_sink;
	viewport = p_viewport;
	width = p_width;
	height = p_height;
	fps = p_fps;
	total_frames = p_total_frames;
	configured = true;
	finished = false;
	operation_error_message = String();
	operation_error = OK;
	RenderingServer::get_singleton()->call_on_render_thread(
			callable_mp(this, &OpenVTDmaBufCapture::_initialize_pipeline_render_thread));
	operation_finished.wait();
	if (operation_error != OK) {
		configured = false;
	}
	return operation_error;
}

Error OpenVTDmaBufCapture::wait_gpu_render(int p_frame_index, bool p_end_of_stream) {
	if (!configured || finished || pipeline == nullptr) {
		operation_error_message = "RGBA direct render target is unavailable";
		return ERR_UNCONFIGURED;
	}
	operation_error = OK;
	RenderingServer::get_singleton()->call_on_render_thread(
			callable_mp(this, &OpenVTDmaBufCapture::_wait_gpu_render_thread)
					.bind(p_frame_index, p_end_of_stream));
	operation_finished.wait();
	return operation_error;
}

Error OpenVTDmaBufCapture::publish_rgba_frame(uint64_t p_timestamp_ns) {
	if (!configured || finished || pipeline == nullptr || p_timestamp_ns == 0) {
		operation_error_message = "RGBA frame is not ready for publication";
		return ERR_INVALID_PARAMETER;
	}
	operation_error = OK;
	RenderingServer::get_singleton()->call_on_render_thread(
			callable_mp(this, &OpenVTDmaBufCapture::_publish_rgba_frame_render_thread)
					.bind(p_timestamp_ns));
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
	String message;
	pipeline = memnew(OpenVTRgbaFramePipeline);
	const Error error = pipeline->initialize(
			rgba_sink, viewport, width, height, fps, total_frames, message);
	if (error != OK) {
		memdelete(pipeline);
		pipeline = nullptr;
	}
	_complete_operation(error, message);
}

void OpenVTDmaBufCapture::_wait_gpu_render_thread(
		int p_frame_index, bool p_end_of_stream) {
	String message;
	_complete_operation(
			pipeline->wait_for_render(p_frame_index, p_end_of_stream, message), message);
}

void OpenVTDmaBufCapture::_publish_rgba_frame_render_thread(uint64_t p_timestamp_ns) {
	String message;
	_complete_operation(pipeline->publish(p_timestamp_ns, message), message);
}

void OpenVTDmaBufCapture::_finish_render_thread() {
	Error error = OK;
	String message;
	if (pipeline != nullptr) {
		error = pipeline->finish(message);
		memdelete(pipeline);
		pipeline = nullptr;
	}
	_complete_operation(error, message);
}

void OpenVTDmaBufCapture::_complete_operation(Error p_error, const String &p_message) {
	operation_error = p_error;
	operation_error_message = p_message;
	if (p_error != OK) {
		ERR_PRINT("OPENVT_RGBA_ERROR " + p_message);
	}
	operation_finished.post();
}
