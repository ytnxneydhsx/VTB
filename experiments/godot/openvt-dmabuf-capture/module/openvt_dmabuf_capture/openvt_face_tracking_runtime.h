#pragma once

#include <memory>

#include "core/object/ref_counted.h"
#include "core/string/ustring.h"
#include "core/variant/dictionary.h"

class OpenVTFaceTrackingRuntimeState;

class OpenVTFaceTrackingRuntime : public RefCounted {
	GDCLASS(OpenVTFaceTrackingRuntime, RefCounted);

public:
	OpenVTFaceTrackingRuntime();
	~OpenVTFaceTrackingRuntime() override;

	OpenVTFaceTrackingRuntime(const OpenVTFaceTrackingRuntime &) = delete;
	// GDCLASS declares a private copy-assignment operator for Object subclasses.
	OpenVTFaceTrackingRuntime(OpenVTFaceTrackingRuntime &&) = delete;
	OpenVTFaceTrackingRuntime &operator=(OpenVTFaceTrackingRuntime &&) = delete;

	Error configure(
			const String &p_source,
			int p_width,
			int p_height,
			int p_fps,
			int p_buffer_count,
			const String &p_detector_model,
			const String &p_landmark_model,
			int p_calibration_frames,
			double p_smoothing_tau_ms,
			double p_angle_x_sign,
			double p_angle_y_sign,
			double p_angle_z_sign,
			const String &p_control_bind_address,
			int p_control_port,
			const String &p_control_allowed_origin);
	Error start();
	Dictionary take_latest();
	void reset_calibration();
	Error stop();
	String get_last_error() const;

protected:
	static void _bind_methods();

private:
	void set_error(const String &p_error);

	std::unique_ptr<OpenVTFaceTrackingRuntimeState> state;
	String last_error;
	bool configured = false;
	bool running = false;
};
