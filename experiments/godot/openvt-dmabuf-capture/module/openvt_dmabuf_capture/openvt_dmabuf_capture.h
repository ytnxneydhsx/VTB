#pragma once

#include "core/object/ref_counted.h"
#include "core/os/semaphore.h"
#include "core/string/ustring.h"
#include "core/templates/rid.h"
#include "vtb/render/rgba_frame_sink.hpp"

class OpenVTRgbaFramePipeline;

class OpenVTDmaBufCapture : public RefCounted {
	GDCLASS(OpenVTDmaBufCapture, RefCounted);

public:
	OpenVTDmaBufCapture() = default;
	~OpenVTDmaBufCapture() override;

	OpenVTDmaBufCapture(const OpenVTDmaBufCapture &) = delete;
	// GDCLASS declares a private copy-assignment operator for Object subclasses.
	OpenVTDmaBufCapture(OpenVTDmaBufCapture &&) = delete;
	OpenVTDmaBufCapture &operator=(OpenVTDmaBufCapture &&) = delete;

	Error configure(
			const vtb::RgbaFrameSink &p_sink,
			const String &p_heap,
			RID p_viewport,
			int p_width,
			int p_height,
			int p_fps,
			int p_total_frames);
	Error acquire_render_slot();
	Error wait_gpu_render(int p_frame_index, bool p_end_of_stream);
	Error publish_rgba_frame(uint64_t p_timestamp_ns);
	Error finish();
	String get_last_error() const;

protected:
	static void _bind_methods();

private:
	void _initialize_pipeline_render_thread();
	void _acquire_render_slot_thread();
	void _wait_gpu_render_thread(int p_frame_index, bool p_end_of_stream);
	void _publish_rgba_frame_render_thread(uint64_t p_timestamp_ns);
	void _finish_render_thread();
	void _complete_operation(Error p_error, const String &p_message);

	vtb::RgbaFrameSink rgba_sink;
	String dma_heap;
	RID viewport;
	int width = 0;
	int height = 0;
	int fps = 0;
	int total_frames = 0;
	bool configured = false;
	bool finished = false;

	OpenVTRgbaFramePipeline *pipeline = nullptr;
	Semaphore operation_finished;
	Error operation_error = OK;
	String operation_error_message;
};

