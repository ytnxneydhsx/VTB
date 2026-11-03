#pragma once

#include "core/object/ref_counted.h"
#include "core/os/semaphore.h"
#include "core/string/ustring.h"
#include "core/templates/rid.h"
#include "vtb/output/encoded_video_sink.hpp"

class OpenVTDmaBufPipeline;

class OpenVTDmaBufCapture : public RefCounted {
	GDCLASS(OpenVTDmaBufCapture, RefCounted);

public:
	OpenVTDmaBufCapture() = default;
	~OpenVTDmaBufCapture() override;

	OpenVTDmaBufCapture(const OpenVTDmaBufCapture &) = delete;
	// GDCLASS declares a private copy-assignment operator for Object subclasses.
	OpenVTDmaBufCapture(OpenVTDmaBufCapture &&) = delete;
	OpenVTDmaBufCapture &operator=(OpenVTDmaBufCapture &&) = delete;

	Error configure(const vtb::EncodedVideoSink &p_sink, int p_width, int p_height, int p_fps, int p_total_frames);
	Error capture_frame(RID p_viewport, int p_frame_index, bool p_end_of_stream);
	Error finish();
	String get_last_error() const;

protected:
	static void _bind_methods();

private:
	void _initialize_pipeline_render_thread();
	void _capture_frame_render_thread(RID p_viewport, int p_slot_index, int p_frame_index, bool p_end_of_stream);
	void _finish_render_thread();
	void _complete_operation(Error p_error, const String &p_message);

	vtb::EncodedVideoSink output_sink;
	int width = 0;
	int height = 0;
	int fps = 0;
	int total_frames = 0;
	bool configured = false;
	bool finished = false;

	OpenVTDmaBufPipeline *pipeline = nullptr;
	Semaphore operation_finished;
	Error operation_error = OK;
	String operation_error_message;
};

