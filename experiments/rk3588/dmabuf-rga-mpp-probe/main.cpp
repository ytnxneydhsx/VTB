#define MODULE_TAG "vtb_dmabuf_probe"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

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

#include <chrono>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

constexpr int kWidth = 1920;
constexpr int kHeight = 1080;
constexpr int kFps = 30;
constexpr int kFrameCount = 300;
constexpr int kRgbaStride = 1920 * 4;
constexpr int kNv12Stride = 1920;
constexpr int kNv12VerticalStride = 1088;
constexpr std::size_t kRgbaSize = static_cast<std::size_t>(kRgbaStride) * kHeight;
constexpr std::size_t kNv12Size = static_cast<std::size_t>(kNv12Stride) * kNv12VerticalStride * 3 / 2;

[[noreturn]] void fail(const std::string &message) {
	throw std::runtime_error(message);
}

void require(bool condition, const std::string &message) {
	if (!condition) {
		fail(message);
	}
}
bool contains_extension(const char *extensions, std::string_view required) {
	if (extensions == nullptr) {
		return false;
	}

	std::string_view all(extensions);
	std::size_t position = 0;
	while (position < all.size()) {
		while (position < all.size() && all[position] == ' ') {
			position++;
		}
		std::size_t end = all.find(' ', position);
		if (end == std::string_view::npos) {
			end = all.size();
		}
		if (all.substr(position, end - position) == required) {
			return true;
		}
		position = end + 1;
	}
	return false;
}

class UniqueFd {
public:
	UniqueFd() = default;

	explicit UniqueFd(int fd) : fd_(fd) {
	}

	~UniqueFd() {
		reset();
	}

	UniqueFd(const UniqueFd &) = delete;
	UniqueFd &operator=(const UniqueFd &) = delete;

	UniqueFd(UniqueFd &&other) noexcept : fd_(other.release()) {
	}

	UniqueFd &operator=(UniqueFd &&other) noexcept {
		if (this != &other) {
			reset(other.release());
		}
		return *this;
	}

	int get() const {
		return fd_;
	}

	int release() {
		int released = fd_;
		fd_ = -1;
		return released;
	}

	void reset(int replacement = -1) {
		if (fd_ >= 0) {
			close(fd_);
		}
		fd_ = replacement;
	}

private:
	int fd_ = -1;
};

class DmaBuffer {
public:
	DmaBuffer(const std::string &heap_path, std::size_t size) : size_(size) {
		UniqueFd heap(open(heap_path.c_str(), O_RDONLY | O_CLOEXEC));
		require(heap.get() >= 0, "open DMA heap failed: " + heap_path + " error=" + std::strerror(errno));

		dma_heap_allocation_data allocation{};
		allocation.len = size_;
		allocation.fd_flags = O_RDWR | O_CLOEXEC;
		allocation.heap_flags = 0;
		int result = ioctl(heap.get(), DMA_HEAP_IOCTL_ALLOC, &allocation);
		require(result == 0, "DMA_HEAP_IOCTL_ALLOC failed: " + std::string(std::strerror(errno)));
		fd_.reset(static_cast<int>(allocation.fd));
	}

	~DmaBuffer() = default;

	DmaBuffer(const DmaBuffer &) = delete;
	DmaBuffer &operator=(const DmaBuffer &) = delete;
	DmaBuffer(DmaBuffer &&) = delete;
	DmaBuffer &operator=(DmaBuffer &&) = delete;

	int fd() const {
		return fd_.get();
	}

	std::size_t size() const {
		return size_;
	}

private:
	UniqueFd fd_;
	std::size_t size_ = 0;
};

class EglContext {
public:
	EglContext() {
		display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
		require(display_ != EGL_NO_DISPLAY, "eglGetDisplay failed");

		EGLint major = 0;
		EGLint minor = 0;
		require(eglInitialize(display_, &major, &minor) == EGL_TRUE, "eglInitialize failed");

		const EGLint config_attributes[] = {
			EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
			EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
			EGL_RED_SIZE, 8,
			EGL_GREEN_SIZE, 8,
			EGL_BLUE_SIZE, 8,
			EGL_ALPHA_SIZE, 8,
			EGL_NONE,
		};
		EGLint config_count = 0;
		require(eglChooseConfig(display_, config_attributes, &config_, 1, &config_count) == EGL_TRUE,
				"eglChooseConfig failed");
		require(config_count == 1, "no matching EGL config");

		const EGLint surface_attributes[] = {
			EGL_WIDTH, 1,
			EGL_HEIGHT, 1,
			EGL_NONE,
		};
		surface_ = eglCreatePbufferSurface(display_, config_, surface_attributes);
		require(surface_ != EGL_NO_SURFACE, "eglCreatePbufferSurface failed");

		const EGLint context_attributes[] = {
			EGL_CONTEXT_CLIENT_VERSION, 2,
			EGL_NONE,
		};
		context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, context_attributes);
		require(context_ != EGL_NO_CONTEXT, "eglCreateContext failed");
		require(eglMakeCurrent(display_, surface_, surface_, context_) == EGL_TRUE, "eglMakeCurrent failed");

		const char *extensions = eglQueryString(display_, EGL_EXTENSIONS);
		require(contains_extension(extensions, "EGL_EXT_image_dma_buf_import"),
				"EGL_EXT_image_dma_buf_import is unavailable");

		create_image_ = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
		destroy_image_ = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
		image_target_texture_ = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
				eglGetProcAddress("glEGLImageTargetTexture2DOES"));
		require(create_image_ != nullptr, "eglCreateImageKHR is unavailable");
		require(destroy_image_ != nullptr, "eglDestroyImageKHR is unavailable");
		require(image_target_texture_ != nullptr, "glEGLImageTargetTexture2DOES is unavailable");

		std::cout << "EGL_VERSION=" << major << "." << minor << '\n';
		std::cout << "EGL_VENDOR=" << eglQueryString(display_, EGL_VENDOR) << '\n';
		std::cout << "GL_VENDOR=" << reinterpret_cast<const char *>(glGetString(GL_VENDOR)) << '\n';
		std::cout << "GL_RENDERER=" << reinterpret_cast<const char *>(glGetString(GL_RENDERER)) << '\n';
		std::cout << "GL_VERSION=" << reinterpret_cast<const char *>(glGetString(GL_VERSION)) << '\n';
	}

	~EglContext() {
		if (display_ != EGL_NO_DISPLAY) {
			eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
			if (context_ != EGL_NO_CONTEXT) {
				eglDestroyContext(display_, context_);
			}
			if (surface_ != EGL_NO_SURFACE) {
				eglDestroySurface(display_, surface_);
			}
			eglTerminate(display_);
		}
	}

	EglContext(const EglContext &) = delete;
	EglContext &operator=(const EglContext &) = delete;
	EglContext(EglContext &&) = delete;
	EglContext &operator=(EglContext &&) = delete;

	EGLDisplay display() const {
		return display_;
	}

	PFNEGLCREATEIMAGEKHRPROC create_image() const {
		return create_image_;
	}

	PFNEGLDESTROYIMAGEKHRPROC destroy_image() const {
		return destroy_image_;
	}

	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture() const {
		return image_target_texture_;
	}

private:
	EGLDisplay display_ = EGL_NO_DISPLAY;
	EGLConfig config_ = nullptr;
	EGLSurface surface_ = EGL_NO_SURFACE;
	EGLContext context_ = EGL_NO_CONTEXT;
	PFNEGLCREATEIMAGEKHRPROC create_image_ = nullptr;
	PFNEGLDESTROYIMAGEKHRPROC destroy_image_ = nullptr;
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture_ = nullptr;
};

class ExternalRenderTarget {
public:
	ExternalRenderTarget(EglContext &egl, const DmaBuffer &buffer) : egl_(egl) {
		const EGLint image_attributes[] = {
			EGL_WIDTH, kWidth,
			EGL_HEIGHT, kHeight,
			EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(DRM_FORMAT_ABGR8888),
			EGL_DMA_BUF_PLANE0_FD_EXT, buffer.fd(),
			EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
			EGL_DMA_BUF_PLANE0_PITCH_EXT, kRgbaStride,
			EGL_NONE,
		};

		image_ = egl_.create_image()(egl_.display(), EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr,
				image_attributes);
		require(image_ != EGL_NO_IMAGE_KHR, "eglCreateImageKHR DMA-BUF import failed");

		glGenTextures(1, &texture_);
		glBindTexture(GL_TEXTURE_2D, texture_);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		egl_.image_target_texture()(GL_TEXTURE_2D, image_);

		glGenFramebuffers(1, &framebuffer_);
		glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture_, 0);
		GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		require(status == GL_FRAMEBUFFER_COMPLETE, "external DMA-BUF framebuffer is incomplete");
	}

	~ExternalRenderTarget() {
		if (framebuffer_ != 0) {
			glDeleteFramebuffers(1, &framebuffer_);
		}
		if (texture_ != 0) {
			glDeleteTextures(1, &texture_);
		}
		if (image_ != EGL_NO_IMAGE_KHR) {
			egl_.destroy_image()(egl_.display(), image_);
		}
	}

	ExternalRenderTarget(const ExternalRenderTarget &) = delete;
	ExternalRenderTarget &operator=(const ExternalRenderTarget &) = delete;
	ExternalRenderTarget(ExternalRenderTarget &&) = delete;
	ExternalRenderTarget &operator=(ExternalRenderTarget &&) = delete;

	void render(int frame_index) {
		float phase = static_cast<float>(frame_index % kFrameCount) / static_cast<float>(kFrameCount);
		glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
		glViewport(0, 0, kWidth, kHeight);
		glDisable(GL_BLEND);
		glEnable(GL_SCISSOR_TEST);

		clear_rect(0, 0, kWidth / 2, kHeight / 2, 1.0F, phase, 0.1F, 1.0F);
		clear_rect(kWidth / 2, 0, kWidth / 2, kHeight / 2, 0.1F, 1.0F, phase, 1.0F);
		clear_rect(0, kHeight / 2, kWidth / 2, kHeight / 2, phase, 0.1F, 1.0F, 1.0F);
		clear_rect(kWidth / 2, kHeight / 2, kWidth / 2, kHeight / 2, 1.0F - phase, phase, 0.5F, 1.0F);

		glDisable(GL_SCISSOR_TEST);
		glFinish();
		require(glGetError() == GL_NO_ERROR, "GLES render into DMA-BUF failed");
	}

private:
	void clear_rect(int x, int y, int width, int height, float red, float green, float blue, float alpha) {
		glScissor(x, y, width, height);
		glClearColor(red, green, blue, alpha);
		glClear(GL_COLOR_BUFFER_BIT);
	}

	EglContext &egl_;
	EGLImageKHR image_ = EGL_NO_IMAGE_KHR;
	GLuint texture_ = 0;
	GLuint framebuffer_ = 0;
};

class RgaConverter {
public:
	RgaConverter(const DmaBuffer &source, const DmaBuffer &destination) {
		source_handle_ = importbuffer_fd(source.fd(), static_cast<int>(source.size()));
		require(source_handle_ != 0, "RGA source DMA-BUF import failed");
		destination_handle_ = importbuffer_fd(destination.fd(), static_cast<int>(destination.size()));
		require(destination_handle_ != 0, "RGA destination DMA-BUF import failed");

		source_buffer_ = wrapbuffer_handle_t(source_handle_, kWidth, kHeight, kRgbaStride / 4, kHeight,
				RK_FORMAT_RGBA_8888);
		destination_buffer_ = wrapbuffer_handle_t(destination_handle_, kWidth, kHeight, kNv12Stride,
				kNv12VerticalStride, RK_FORMAT_YCbCr_420_SP);

		im_rect source_rect{};
		im_rect destination_rect{};
		rga_buffer_t pattern_buffer{};
		im_rect pattern_rect{};
		IM_STATUS check = imcheck_t(source_buffer_, destination_buffer_, pattern_buffer, source_rect,
				destination_rect, pattern_rect, 0);
		require(check == IM_STATUS_NOERROR || check == IM_STATUS_SUCCESS,
				"RGA imcheck failed: " + std::string(imStrError_t(check)));
	}

	~RgaConverter() {
		if (destination_handle_ != 0) {
			releasebuffer_handle(destination_handle_);
		}
		if (source_handle_ != 0) {
			releasebuffer_handle(source_handle_);
		}
	}

	RgaConverter(const RgaConverter &) = delete;
	RgaConverter &operator=(const RgaConverter &) = delete;
	RgaConverter(RgaConverter &&) = delete;
	RgaConverter &operator=(RgaConverter &&) = delete;

	void convert() {
		IM_STATUS status = imcvtcolor_t(source_buffer_, destination_buffer_, RK_FORMAT_RGBA_8888,
				RK_FORMAT_YCbCr_420_SP, IM_RGB_TO_YUV_BT601_LIMIT, 1);
		require(status == IM_STATUS_SUCCESS, "RGA imcvtcolor failed: " + std::string(imStrError_t(status)));
	}

private:
	rga_buffer_handle_t source_handle_ = 0;
	rga_buffer_handle_t destination_handle_ = 0;
	rga_buffer_t source_buffer_{};
	rga_buffer_t destination_buffer_{};
};

class MppEncoder {
public:
	MppEncoder(const DmaBuffer &input, const std::string &output_path) : output_(output_path, std::ios::binary) {
		require(output_.is_open(), "open output stream failed: " + output_path);

		MPP_RET result = mpp_create(&context_, &api_);
		require(result == MPP_OK, "mpp_create failed: " + std::to_string(result));

		MppPollType blocking = MPP_POLL_BLOCK;
		result = api_->control(context_, MPP_SET_OUTPUT_TIMEOUT, &blocking);
		require(result == MPP_OK, "MPP_SET_OUTPUT_TIMEOUT failed: " + std::to_string(result));

		result = mpp_init(context_, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);
		require(result == MPP_OK, "mpp_init AVC encoder failed: " + std::to_string(result));

		result = mpp_enc_cfg_init(&config_);
		require(result == MPP_OK, "mpp_enc_cfg_init failed: " + std::to_string(result));
		result = api_->control(context_, MPP_ENC_GET_CFG, config_);
		require(result == MPP_OK, "MPP_ENC_GET_CFG failed: " + std::to_string(result));

		set_config("prep:width", kWidth);
		set_config("prep:height", kHeight);
		set_config("prep:hor_stride", kNv12Stride);
		set_config("prep:ver_stride", kNv12VerticalStride);
		set_config("prep:format", MPP_FMT_YUV420SP);
		set_config("rc:mode", MPP_ENC_RC_MODE_CBR);
		set_config("rc:fps_in_flex", 0);
		set_config("rc:fps_in_num", kFps);
		set_config("rc:fps_in_denorm", 1);
		set_config("rc:fps_out_flex", 0);
		set_config("rc:fps_out_num", kFps);
		set_config("rc:fps_out_denorm", 1);
		set_config("rc:gop", kFps * 2);
		set_config("rc:bps_target", 8'000'000);
		set_config("rc:bps_max", 8'500'000);
		set_config("rc:bps_min", 7'500'000);
		set_config("codec:type", MPP_VIDEO_CodingAVC);
		set_config("h264:profile", 100);
		set_config("h264:level", 40);
		set_config("h264:cabac_en", 1);
		set_config("h264:cabac_idc", 0);
		set_config("h264:trans8x8", 1);

		result = api_->control(context_, MPP_ENC_SET_CFG, config_);
		require(result == MPP_OK, "MPP_ENC_SET_CFG failed: " + std::to_string(result));

		MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
		result = api_->control(context_, MPP_ENC_SET_HEADER_MODE, &header_mode);
		require(result == MPP_OK, "MPP_ENC_SET_HEADER_MODE failed: " + std::to_string(result));

		MppBufferInfo input_info{};
		input_info.type = MPP_BUFFER_TYPE_EXT_DMA;
		input_info.size = input.size();
		input_info.fd = input.fd();
		input_info.index = -1;
		result = mpp_buffer_import(&input_buffer_, &input_info);
		require(result == MPP_OK, "mpp_buffer_import NV12 DMA-BUF failed: " + std::to_string(result));

		result = mpp_frame_init(&frame_);
		require(result == MPP_OK, "mpp_frame_init failed: " + std::to_string(result));
		mpp_frame_set_width(frame_, kWidth);
		mpp_frame_set_height(frame_, kHeight);
		mpp_frame_set_hor_stride(frame_, kNv12Stride);
		mpp_frame_set_ver_stride(frame_, kNv12VerticalStride);
		mpp_frame_set_fmt(frame_, MPP_FMT_YUV420SP);
		mpp_frame_set_buffer(frame_, input_buffer_);
	}

	~MppEncoder() {
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
	}

	MppEncoder(const MppEncoder &) = delete;
	MppEncoder &operator=(const MppEncoder &) = delete;
	MppEncoder(MppEncoder &&) = delete;
	MppEncoder &operator=(MppEncoder &&) = delete;

	void encode_frame(int frame_index, bool end_of_stream) {
		mpp_frame_set_pts(frame_, static_cast<RK_S64>(frame_index) * 1'000'000 / kFps);
		RK_U32 eos_value = 0;
		if (end_of_stream) {
			eos_value = 1;
		}
		mpp_frame_set_eos(frame_, eos_value);

		MPP_RET result = api_->encode_put_frame(context_, frame_);
		require(result == MPP_OK, "encode_put_frame failed: " + std::to_string(result));

		MppPacket packet = nullptr;
		result = api_->encode_get_packet(context_, &packet);
		require(result == MPP_OK, "encode_get_packet failed: " + std::to_string(result));
		require(packet != nullptr, "encoder returned no packet");

		void *position = mpp_packet_get_pos(packet);
		std::size_t length = mpp_packet_get_length(packet);
		require(position != nullptr, "encoder packet has no data");
		require(length > 0, "encoder packet is empty");
		output_.write(static_cast<const char *>(position), static_cast<std::streamsize>(length));
		require(output_.good(), "write encoded packet failed");
		encoded_bytes_ += length;
		mpp_packet_deinit(&packet);
	}

	std::size_t encoded_bytes() const {
		return encoded_bytes_;
	}

private:
	void set_config(const char *name, RK_S32 value) {
		MPP_RET result = mpp_enc_cfg_set_s32(config_, name, value);
		require(result == MPP_OK, "mpp_enc_cfg_set_s32 failed for " + std::string(name) + ": " +
				std::to_string(result));
	}

	MppCtx context_ = nullptr;
	MppApi *api_ = nullptr;
	MppEncCfg config_ = nullptr;
	MppBuffer input_buffer_ = nullptr;
	MppFrame frame_ = nullptr;
	std::ofstream output_;
	std::size_t encoded_bytes_ = 0;
};

double milliseconds(const std::chrono::steady_clock::duration &duration) {
	return std::chrono::duration<double, std::milli>(duration).count();
}

} // namespace

int main(int argc, char **argv) {
	try {
		std::string output_path = "/tmp/dmabuf-rga-mpp-probe.h264";
		if (argc >= 2) {
			output_path = argv[1];
		}
		std::string heap_path = "/dev/dma_heap/system-uncached";
		if (argc >= 3) {
			heap_path = argv[2];
		}

		std::cout << "PROBE_CONFIG width=" << kWidth << " height=" << kHeight << " fps=" << kFps
				<< " frames=" << kFrameCount << " rgba_stride=" << kRgbaStride
				<< " nv12_stride=" << kNv12Stride << " nv12_ver_stride=" << kNv12VerticalStride
				<< " dma_heap=" << heap_path << '\n';

		DmaBuffer rgba_buffer(heap_path, kRgbaSize);
		DmaBuffer nv12_buffer(heap_path, kNv12Size);
		std::cout << "DMA_BUFFER rgba_fd=" << rgba_buffer.fd() << " rgba_size=" << rgba_buffer.size()
				<< " nv12_fd=" << nv12_buffer.fd() << " nv12_size=" << nv12_buffer.size() << '\n';
		std::cout << "DMA_LAYOUT rgba_drm_format=AB24 rgba_rga_format=RK_FORMAT_RGBA_8888"
				<< " nv12_rga_format=RK_FORMAT_YCbCr_420_SP modifier=not-specified" << '\n';

		EglContext egl;
		ExternalRenderTarget render_target(egl, rgba_buffer);
		RgaConverter converter(rgba_buffer, nv12_buffer);
		MppEncoder encoder(nv12_buffer, output_path);

		double render_total_ms = 0.0;
		double rga_total_ms = 0.0;
		double mpp_total_ms = 0.0;
		auto total_start = std::chrono::steady_clock::now();

		for (int frame = 0; frame < kFrameCount; frame++) {
			auto render_start = std::chrono::steady_clock::now();
			render_target.render(frame);
			auto render_end = std::chrono::steady_clock::now();
			converter.convert();
			auto rga_end = std::chrono::steady_clock::now();
			bool end_of_stream = frame + 1 == kFrameCount;
			encoder.encode_frame(frame, end_of_stream);
			auto mpp_end = std::chrono::steady_clock::now();

			render_total_ms += milliseconds(render_end - render_start);
			rga_total_ms += milliseconds(rga_end - render_end);
			mpp_total_ms += milliseconds(mpp_end - rga_end);

			if (frame == 0 || (frame + 1) % kFps == 0) {
				std::cout << "PROBE_FRAME frame=" << frame + 1 << " render_ms="
						<< milliseconds(render_end - render_start) << " rga_ms="
						<< milliseconds(rga_end - render_end) << " mpp_ms="
						<< milliseconds(mpp_end - rga_end) << '\n';
			}
		}

		auto total_end = std::chrono::steady_clock::now();
		double total_ms = milliseconds(total_end - total_start);
		std::cout << std::fixed << std::setprecision(3);
		std::cout << "PROBE_RESULT frames=" << kFrameCount << " total_ms=" << total_ms
				<< " throughput_fps=" << static_cast<double>(kFrameCount) * 1000.0 / total_ms
				<< " render_avg_ms=" << render_total_ms / kFrameCount
				<< " rga_avg_ms=" << rga_total_ms / kFrameCount
				<< " mpp_avg_ms=" << mpp_total_ms / kFrameCount
				<< " encoded_bytes=" << encoder.encoded_bytes() << " output=" << output_path << '\n';
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "PROBE_ERROR " << error.what() << '\n';
		return 1;
	}
}
