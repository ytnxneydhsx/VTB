#include "openvt_libgodot_bridge.h"

#include <memory>
#include <string>

#include "core/extension/godot_instance.h"
#include "embedded_live2d_application.h"

namespace {

GodotInstance *instance_from_opaque(void *p_instance) {
	return static_cast<GodotInstance *>(p_instance);
}

std::unique_ptr<vtb::EmbeddedLive2DApplication> application;
thread_local std::string application_error;

int remember_application_error(Error p_error) {
	if (p_error == OK) {
		application_error.clear();
		return static_cast<int>(p_error);
	}
	if (application != nullptr) {
		application_error = application->last_error().utf8().get_data();
	}
	return static_cast<int>(p_error);
}

} // namespace

extern "C" int openvt_libgodot_start(void *p_instance) {
	GodotInstance *instance = instance_from_opaque(p_instance);
	if (instance == nullptr) {
		return -1;
	}
	if (!instance->start()) {
		return -2;
	}
	return 0;
}

extern "C" int openvt_libgodot_is_started(void *p_instance) {
	GodotInstance *instance = instance_from_opaque(p_instance);
	if (instance == nullptr) {
		return 0;
	}
	if (instance->is_started()) {
		return 1;
	}
	return 0;
}

extern "C" int openvt_libgodot_iteration(void *p_instance) {
	GodotInstance *instance = instance_from_opaque(p_instance);
	if (instance == nullptr || !instance->is_started()) {
		return -1;
	}
	if (instance->iteration()) {
		return 1;
	}
	return 0;
}

extern "C" int openvt_libgodot_application_initialize(void) {
	if (application != nullptr) {
		application_error = "Embedded Live2D application is already initialized";
		return static_cast<int>(ERR_ALREADY_IN_USE);
	}
	application = std::make_unique<vtb::EmbeddedLive2DApplication>();
	const Error error = application->initialize_from_environment();
	if (error != OK) {
		const int result = remember_application_error(error);
		application->shutdown();
		application.reset();
		return result;
	}
	application_error.clear();
	return static_cast<int>(OK);
}

extern "C" int openvt_libgodot_application_prepare_frame(uint64_t p_monotonic_timestamp_ns) {
	if (application == nullptr) {
		application_error = "Embedded Live2D application is unavailable";
		return static_cast<int>(ERR_UNCONFIGURED);
	}
	return remember_application_error(application->prepare_frame(p_monotonic_timestamp_ns));
}

extern "C" int openvt_libgodot_application_complete_frame(void) {
	if (application == nullptr) {
		application_error = "Embedded Live2D application is unavailable";
		return static_cast<int>(ERR_UNCONFIGURED);
	}
	return remember_application_error(application->complete_frame());
}

extern "C" int openvt_libgodot_application_should_stop(void) {
	if (application == nullptr) {
		return 1;
	}
	if (application->should_stop()) {
		return 1;
	}
	return 0;
}

extern "C" uint64_t openvt_libgodot_application_completed_frames(void) {
	if (application == nullptr) {
		return 0;
	}
	return application->completed_frames();
}

extern "C" int openvt_libgodot_application_shutdown(void) {
	if (application == nullptr) {
		return static_cast<int>(OK);
	}
	const Error error = application->shutdown();
	const int result = remember_application_error(error);
	application.reset();
	return result;
}

extern "C" const char *openvt_libgodot_application_last_error(void) {
	return application_error.c_str();
}

extern "C" void openvt_libgodot_bridge_force_link(void) {
}
