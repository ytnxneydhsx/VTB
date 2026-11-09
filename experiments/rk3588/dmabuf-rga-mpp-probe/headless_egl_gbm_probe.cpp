#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>

struct gbm_device;

extern "C" gbm_device *gbm_create_device(int p_fd);
extern "C" void gbm_device_destroy(gbm_device *p_device);

namespace {

void EGLAPIENTRY set_blob_cache(const void *p_key, EGLsizeiANDROID p_key_size, const void *p_value,
		EGLsizeiANDROID p_value_size) {
	static_cast<void>(p_key);
	static_cast<void>(p_key_size);
	static_cast<void>(p_value);
	static_cast<void>(p_value_size);
}

EGLsizeiANDROID EGLAPIENTRY get_blob_cache(const void *p_key, EGLsizeiANDROID p_key_size, void *p_value,
		EGLsizeiANDROID p_value_size) {
	static_cast<void>(p_key);
	static_cast<void>(p_key_size);
	static_cast<void>(p_value);
	static_cast<void>(p_value_size);
	return 0;
}

class GbmEglContext {
public:
	GbmEglContext(const char *p_drm_path, bool p_terminate_query_only, bool p_use_godot_config) {
		std::cerr << "opening DRM node: " << p_drm_path << std::endl;
		drm_fd = open(p_drm_path, O_RDWR | O_CLOEXEC);
		if (drm_fd < 0) {
			std::cerr << "open DRM node failed: " << std::strerror(errno) << '\n';
			return;
		}

		std::cerr << "creating GBM device" << std::endl;
		gbm = gbm_create_device(drm_fd);
		if (gbm == nullptr) {
			std::cerr << "gbm_create_device failed\n";
			return;
		}

		std::cerr << "getting EGL display" << std::endl;
		PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
				reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
		if (get_platform_display == nullptr) {
			std::cerr << "eglGetPlatformDisplayEXT is unavailable\n";
			return;
		}
		display = get_platform_display(EGL_PLATFORM_GBM_KHR, gbm, nullptr);
		if (display == EGL_NO_DISPLAY) {
			std::cerr << "eglGetDisplay(gbm_device) failed: 0x" << std::hex << eglGetError() << '\n';
			return;
		}

		std::cerr << "initializing EGL display" << std::endl;
		if (eglInitialize(display, &egl_major, &egl_minor) != EGL_TRUE) {
			std::cerr << "eglInitialize failed: 0x" << std::hex << eglGetError() << '\n';
			return;
		}
		if (p_terminate_query_only) {
			std::cerr << "terminating temporary EGL display" << std::endl;
			eglTerminate(display);
			display = EGL_NO_DISPLAY;
			std::cerr << "querying client EGL extensions" << std::endl;
			const char *client_extensions = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
			if (client_extensions == nullptr) {
				std::cerr << "client EGL extension query failed: 0x" << std::hex << eglGetError() << '\n';
				return;
			}
			std::cout << "EGL_CLIENT_EXTENSIONS=" << client_extensions << '\n';
			std::cerr << "getting the permanent EGL display" << std::endl;
			display = get_platform_display(EGL_PLATFORM_GBM_KHR, gbm, nullptr);
			if (display == EGL_NO_DISPLAY) {
				std::cerr << "second eglGetPlatformDisplayEXT failed: 0x" << std::hex << eglGetError() << '\n';
				return;
			}
			std::cerr << "initializing the permanent EGL display" << std::endl;
			if (eglInitialize(display, &egl_major, &egl_minor) != EGL_TRUE) {
				std::cerr << "second eglInitialize failed: 0x" << std::hex << eglGetError() << '\n';
				return;
			}
			std::cout << "SECOND_EGL_VERSION=" << egl_major << '.' << egl_minor << '\n';
		}

		const EGLint standard_config_attributes[] = {
			EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
			EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
			EGL_RED_SIZE, 8,
			EGL_GREEN_SIZE, 8,
			EGL_BLUE_SIZE, 8,
			EGL_ALPHA_SIZE, 8,
			EGL_NONE,
		};
		const EGLint godot_config_attributes[] = {
			EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
			EGL_RED_SIZE, 1,
			EGL_BLUE_SIZE, 1,
			EGL_GREEN_SIZE, 1,
			EGL_DEPTH_SIZE, 24,
			EGL_NONE,
		};
		const EGLint *config_attributes = standard_config_attributes;
		if (p_use_godot_config) {
			std::cerr << "using Godot EGLConfig attributes" << std::endl;
			config_attributes = godot_config_attributes;
		}
		EGLint config_count = 0;
		if (eglChooseConfig(display, config_attributes, &config, 1, &config_count) != EGL_TRUE || config_count != 1) {
			std::cerr << "eglChooseConfig failed: 0x" << std::hex << eglGetError() << '\n';
			return;
		}

		const EGLint surface_attributes[] = {
			EGL_WIDTH, 1,
			EGL_HEIGHT, 1,
			EGL_NONE,
		};
		surface = eglCreatePbufferSurface(display, config, surface_attributes);
		if (surface == EGL_NO_SURFACE) {
			std::cerr << "eglCreatePbufferSurface failed: 0x" << std::hex << eglGetError() << '\n';
			return;
		}

		const EGLint context_attributes[] = {
			EGL_CONTEXT_CLIENT_VERSION, 3,
			EGL_NONE,
		};
		context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
		if (context == EGL_NO_CONTEXT) {
			std::cerr << "eglCreateContext failed: 0x" << std::hex << eglGetError() << '\n';
			return;
		}
		if (p_terminate_query_only) {
			std::cerr << "registering EGL blob cache callbacks" << std::endl;
			PFNEGLSETBLOBCACHEFUNCSANDROIDPROC set_blob_cache_functions =
					reinterpret_cast<PFNEGLSETBLOBCACHEFUNCSANDROIDPROC>(
							eglGetProcAddress("eglSetBlobCacheFuncsANDROID"));
			if (set_blob_cache_functions == nullptr) {
				std::cerr << "eglSetBlobCacheFuncsANDROID is unavailable\n";
				return;
			}
			set_blob_cache_functions(display, set_blob_cache, get_blob_cache);
		}

		if (eglMakeCurrent(display, surface, surface, context) != EGL_TRUE) {
			std::cerr << "eglMakeCurrent failed: 0x" << std::hex << eglGetError() << '\n';
			return;
		}

		initialized = true;
	}

	~GbmEglContext() {
		if (display != EGL_NO_DISPLAY) {
			eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
			if (context != EGL_NO_CONTEXT) {
				eglDestroyContext(display, context);
			}
			if (surface != EGL_NO_SURFACE) {
				eglDestroySurface(display, surface);
			}
			eglTerminate(display);
		}
		if (gbm != nullptr) {
			gbm_device_destroy(gbm);
		}
		if (drm_fd >= 0) {
			close(drm_fd);
		}
	}

	GbmEglContext(const GbmEglContext &) = delete;
	GbmEglContext &operator=(const GbmEglContext &) = delete;
	GbmEglContext(GbmEglContext &&) = delete;
	GbmEglContext &operator=(GbmEglContext &&) = delete;

	bool is_initialized() const {
		return initialized;
	}

	void print_details() const {
		std::cout << "EGL_VERSION=" << egl_major << '.' << egl_minor << '\n';
		std::cout << "EGL_VENDOR=" << eglQueryString(display, EGL_VENDOR) << '\n';
		std::cout << "GL_VENDOR=" << reinterpret_cast<const char *>(glGetString(GL_VENDOR)) << '\n';
		std::cout << "GL_RENDERER=" << reinterpret_cast<const char *>(glGetString(GL_RENDERER)) << '\n';
		std::cout << "GL_VERSION=" << reinterpret_cast<const char *>(glGetString(GL_VERSION)) << '\n';
	}

private:
	int drm_fd = -1;
	gbm_device *gbm = nullptr;
	EGLDisplay display = EGL_NO_DISPLAY;
	EGLConfig config = nullptr;
	EGLSurface surface = EGL_NO_SURFACE;
	EGLContext context = EGL_NO_CONTEXT;
	EGLint egl_major = 0;
	EGLint egl_minor = 0;
	bool initialized = false;
};

} // namespace

int main(int p_argc, char **p_argv) {
	const char *drm_path = "/dev/dri/card0";
	bool terminate_query_only = false;
	bool use_godot_config = false;
	if (p_argc >= 2) {
		drm_path = p_argv[1];
	}
	for (int argument_index = 2; argument_index < p_argc; ++argument_index) {
		if (std::strcmp(p_argv[argument_index], "--terminate-query") == 0) {
			terminate_query_only = true;
		}
		if (std::strcmp(p_argv[argument_index], "--godot-config") == 0) {
			use_godot_config = true;
		}
	}
	GbmEglContext context(drm_path, terminate_query_only, use_godot_config);
	if (!context.is_initialized()) {
		return 1;
	}
	if (!terminate_query_only) {
		context.print_details();
	}
	return 0;
}
