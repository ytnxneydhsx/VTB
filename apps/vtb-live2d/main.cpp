#include <atomic>
#include <chrono>
#include <cstdint>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "core/extension/libgodot.h"
#include "openvt_libgodot_bridge.h"

namespace {

std::atomic<bool> stop_requested{ false };
static_assert(std::atomic<bool>::is_always_lock_free,
		"The signal stop flag must use lock-free atomic operations");

void handle_signal(int) noexcept {
	stop_requested.store(true, std::memory_order_release);
}

void initialize_host_extension(
		void *,
		GDExtensionInitializationLevel) {
}

void deinitialize_host_extension(
		void *,
		GDExtensionInitializationLevel) {
}

GDExtensionBool host_extension_init(
		GDExtensionInterfaceGetProcAddress,
		GDExtensionClassLibraryPtr,
		GDExtensionInitialization *r_initialization) {
	if (r_initialization == nullptr) {
		return 0;
	}
	r_initialization->minimum_initialization_level = GDEXTENSION_INITIALIZATION_CORE;
	r_initialization->userdata = nullptr;
	r_initialization->initialize = initialize_host_extension;
	r_initialization->deinitialize = deinitialize_host_extension;
	return 1;
}

class ApplicationArguments final {
public:
	ApplicationArguments(int p_argc, char **p_argv) {
		if (p_argc < 1 || p_argv == nullptr || p_argv[0] == nullptr) {
			throw std::invalid_argument("Invalid process arguments");
		}
		godot_arguments_.emplace_back(p_argv[0]);
		for (int index = 1; index < p_argc; ++index) {
			const std::string argument(p_argv[index]);
			if (argument == "--vtb-iterations") {
				if (index + 1 >= p_argc) {
					throw std::invalid_argument("--vtb-iterations requires a value");
				}
				iteration_limit_ = parse_positive_integer(p_argv[++index]);
				continue;
			}
			godot_arguments_.push_back(argument);
		}
		if (!has_argument("--path")) {
			const char *project_root = std::getenv("OPENVT_GODOT_PROJECT_DIR");
			if (project_root == nullptr || project_root[0] == '\0') {
				throw std::invalid_argument(
						"OPENVT_GODOT_PROJECT_DIR or --path is required");
			}
			godot_arguments_.emplace_back("--path");
			godot_arguments_.emplace_back(project_root);
		}
		if (!has_argument("--display-driver")) {
			godot_arguments_.emplace_back("--display-driver");
			godot_arguments_.emplace_back("x11");
		}
		if (!has_argument("--rendering-driver")) {
			godot_arguments_.emplace_back("--rendering-driver");
			godot_arguments_.emplace_back("opengl3_es");
		}
		if (!has_argument("--single-window")) {
			godot_arguments_.emplace_back("--single-window");
		}
		if (!has_argument("--disable-crash-handler")) {
			godot_arguments_.emplace_back("--disable-crash-handler");
		}
		if (!has_argument("--disable-vsync")) {
			godot_arguments_.emplace_back("--disable-vsync");
		}
		if (!has_argument("--fixed-fps")) {
			godot_arguments_.emplace_back("--fixed-fps");
			const char *output_fps = std::getenv("OPENVT_OUTPUT_FPS");
			if (output_fps == nullptr || output_fps[0] == '\0') {
				godot_arguments_.emplace_back("30");
				output_fps_ = 30;
			} else {
				output_fps_ = parse_positive_integer(output_fps);
				if (output_fps_ > 1000) {
					throw std::invalid_argument("OPENVT_OUTPUT_FPS must not exceed 1000");
				}
				godot_arguments_.emplace_back(output_fps);
			}
		} else {
			const char *output_fps = std::getenv("OPENVT_OUTPUT_FPS");
			if (output_fps != nullptr && output_fps[0] != '\0') {
				output_fps_ = parse_positive_integer(output_fps);
			}
		}
		refresh_pointers();
	}

	~ApplicationArguments() = default;

	ApplicationArguments(const ApplicationArguments &) = delete;
	ApplicationArguments &operator=(const ApplicationArguments &) = delete;
	ApplicationArguments(ApplicationArguments &&) = delete;
	ApplicationArguments &operator=(ApplicationArguments &&) = delete;

	int godot_argument_count() const noexcept {
		return static_cast<int>(godot_argument_pointers_.size());
	}

	char **godot_arguments() noexcept {
		return godot_argument_pointers_.data();
	}

	unsigned long iteration_limit() const noexcept {
		return iteration_limit_;
	}

	std::chrono::nanoseconds frame_period() const noexcept {
		return std::chrono::nanoseconds(1000000000ULL / output_fps_);
	}

private:
	static unsigned long parse_positive_integer(const char *p_text) {
		if (p_text == nullptr || p_text[0] == '\0') {
			throw std::invalid_argument("Iteration limit is empty");
		}
		errno = 0;
		char *end = nullptr;
		const unsigned long value = std::strtoul(p_text, &end, 10);
		if (errno == ERANGE || end == p_text || end == nullptr || end[0] != '\0' || value == 0) {
			throw std::invalid_argument("Iteration limit must be a positive integer");
		}
		return value;
	}

	bool has_argument(const std::string &p_name) const {
		for (const std::string &argument : godot_arguments_) {
			if (argument == p_name) {
				return true;
			}
		}
		return false;
	}

	void refresh_pointers() {
		godot_argument_pointers_.clear();
		godot_argument_pointers_.reserve(godot_arguments_.size());
		for (std::string &argument : godot_arguments_) {
			godot_argument_pointers_.push_back(argument.data());
		}
	}

	std::vector<std::string> godot_arguments_;
	std::vector<char *> godot_argument_pointers_;
	unsigned long iteration_limit_ = 0;
	unsigned long output_fps_ = 30;
};

class LibGodotInstance final {
public:
	explicit LibGodotInstance(ApplicationArguments &p_arguments) {
		instance_ = libgodot_create_godot_instance(
				p_arguments.godot_argument_count(),
				p_arguments.godot_arguments(),
				host_extension_init);
		if (instance_ == nullptr) {
			throw std::runtime_error("LibGodot instance creation failed");
		}
	}

	~LibGodotInstance() {
		shutdown();
	}

	LibGodotInstance(const LibGodotInstance &) = delete;
	LibGodotInstance &operator=(const LibGodotInstance &) = delete;
	LibGodotInstance(LibGodotInstance &&) = delete;
	LibGodotInstance &operator=(LibGodotInstance &&) = delete;

	void start() {
		const int result = openvt_libgodot_start(instance_);
		if (result != 0) {
			throw std::runtime_error("LibGodot start failed: " + std::to_string(result));
		}
		const int application_result = openvt_libgodot_application_initialize();
		if (application_result != 0) {
			throw std::runtime_error(
					"C++ Live2D application initialization failed: " +
					std::to_string(application_result) + " " +
					openvt_libgodot_application_last_error());
		}
		application_initialized_ = true;
	}

	bool run_frame() {
		const int prepare_result = openvt_libgodot_application_prepare_frame(monotonic_now_ns());
		if (prepare_result != 0) {
			throw std::runtime_error(
					"C++ Live2D frame preparation failed: " +
					std::to_string(prepare_result) + " " +
					openvt_libgodot_application_last_error());
		}
		const int result = openvt_libgodot_iteration(instance_);
		if (result < 0) {
			throw std::runtime_error("LibGodot iteration failed: " + std::to_string(result));
		}
		const int complete_result = openvt_libgodot_application_complete_frame();
		if (complete_result != 0) {
			throw std::runtime_error(
					"C++ Live2D frame completion failed: " +
					std::to_string(complete_result) + " " +
					openvt_libgodot_application_last_error());
		}
		if (result > 0 || openvt_libgodot_application_should_stop() != 0) {
			return true;
		}
		return false;
	}

	int shutdown() noexcept {
		int application_result = 0;
		if (application_initialized_) {
			application_result = openvt_libgodot_application_shutdown();
			application_initialized_ = false;
		}
		if (instance_ != nullptr) {
			libgodot_destroy_godot_instance(instance_);
			instance_ = nullptr;
		}
		return application_result;
	}

private:
	static uint64_t monotonic_now_ns() noexcept {
		const auto now = std::chrono::steady_clock::now().time_since_epoch();
		return static_cast<uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
	}

	GDExtensionObjectPtr instance_ = nullptr;
	bool application_initialized_ = false;
};

} // namespace

int main(int argc, char **argv) {
	try {
		ApplicationArguments arguments(argc, argv);
		std::signal(SIGINT, handle_signal);
		std::signal(SIGTERM, handle_signal);

		LibGodotInstance godot(arguments);
		godot.start();

		unsigned long iterations = 0;
		auto next_frame_deadline = std::chrono::steady_clock::now();
		while (!stop_requested.load(std::memory_order_acquire)) {
			const bool finished = godot.run_frame();
			++iterations;
			if (finished) {
				break;
			}
			if (arguments.iteration_limit() > 0 &&
					iterations >= arguments.iteration_limit()) {
				break;
			}
			next_frame_deadline += arguments.frame_period();
			const auto current_time = std::chrono::steady_clock::now();
			if (next_frame_deadline < current_time) {
				next_frame_deadline = current_time;
			}
			std::this_thread::sleep_until(next_frame_deadline);
		}

		const uint64_t completed_frames =
				openvt_libgodot_application_completed_frames();
		std::cout << "VTB_LIBGODOT_RESULT iterations=" << iterations
				  << " completed_frames="
				  << completed_frames << "\n";
		const int shutdown_result = godot.shutdown();
		if (shutdown_result != 0) {
			throw std::runtime_error(
					"C++ Live2D application shutdown failed: " +
					std::to_string(shutdown_result) + " " +
					openvt_libgodot_application_last_error());
		}
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "VTB_LIBGODOT_ERROR " << error.what() << "\n";
		return 1;
	}
}
