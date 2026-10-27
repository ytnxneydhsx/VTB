#include "register_types.h"

#include "openvt_dmabuf_capture.h"

#include "core/object/class_db.h"

void initialize_openvt_dmabuf_capture_module(ModuleInitializationLevel p_level) {
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

