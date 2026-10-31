#include "register_types.h"

#include "openvt_dmabuf_capture.h"
#include "openvt_libgodot_bridge.h"

#include "core/object/class_db.h"

void initialize_openvt_dmabuf_capture_module(ModuleInitializationLevel p_level) {
	openvt_libgodot_bridge_force_link();
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	GDREGISTER_CLASS(OpenVTDmaBufCapture);
}

void uninitialize_openvt_dmabuf_capture_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
}

