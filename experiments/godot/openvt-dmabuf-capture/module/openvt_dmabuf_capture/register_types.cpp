#include "register_types.h"

#include "openvt_dmabuf_capture.h"
#include "openvt_face_tracking_runtime.h"

#include "core/object/class_db.h"

void initialize_openvt_dmabuf_capture_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	GDREGISTER_CLASS(OpenVTDmaBufCapture);
	GDREGISTER_CLASS(OpenVTFaceTrackingRuntime);
}

void uninitialize_openvt_dmabuf_capture_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
}

