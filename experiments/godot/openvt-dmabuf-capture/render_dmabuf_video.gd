extends Node2D

const Live2DTrackingReceiver := preload("res://experiments/openvt-dmabuf-capture/live2d_tracking_receiver.gd")
const MODEL_PATH := "/home/huangnv/open-vt-model/阿芙洛狄忒模型文件/fense_vts/fense.model3.json"
const OUTPUT_SIZE := Vector2i(1920, 1080)
const OUTPUT_FPS := 30
const DEFAULT_TOTAL_FRAMES := 300
const MOTION_START_FRAME := 210
const SHAKE_MOTION := &"yaotou.motion3.json"
const TRACKING_MAX_AGE_USEC := 150000
const TRACKING_FALLBACK_TAU_SECONDS := 0.25
const DEFAULT_PROFILE_REPORT_FRAMES := 300
const VIEW_ZOOM_MIN := 0.5
const VIEW_ZOOM_MAX := 6.0
const VIEW_OFFSET_MIN := -2.0
const VIEW_OFFSET_MAX := 2.0
const TRACKED_FACE_PARAMETERS: Array[StringName] = [
	&"ParamAngleX",
	&"ParamAngleY",
	&"ParamAngleZ",
	&"ParamBodyAngleX",
	&"ParamBodyAngleY",
	&"ParamBodyAngleZ",
	&"ParamEyeLOpen",
	&"ParamEyeROpen",
	&"ParamMouthOpenY",
	&"ParamMouthForm",
]

var model: AyagamiModel
var motion_controller: AnimationPlayer
var render_viewport: SubViewport
var dma_capture
var frame_index := 0
var capture_start_usec := 0
var finishing := false
var capture_pending := false
var profile_enabled := false
var profile_animation_usec := 0
var profile_draw_wait_usec := 0
var profile_capture_usec := 0
var profile_draw_wait_started_usec := 0
var available_parameters := {}
var tracking_receiver
var tracking_enabled := false
var tracking_last_packet_usec := 0
var tracking_last_calibrated := false
var tracking_active_source_key := ""
var tracking_last_source_frame := 0
var tracking_last_associated_render_frame := 0
var tracking_profile_total := {}
var tracking_profile_window := {}
var tracking_profile_final_printed := false
var profile_report_frames := DEFAULT_PROFILE_REPORT_FRAMES
var total_frames := DEFAULT_TOTAL_FRAMES
var base_model_scale := 1.0
var view_zoom := 1.0
var view_offset := Vector2.ZERO


func _ready() -> void:
	Engine.max_fps = OUTPUT_FPS
	RenderingServer.set_default_clear_color(Color(0.18, 0.18, 0.18, 1.0))
	tracking_profile_total = _empty_tracking_profile()
	tracking_profile_window = _empty_tracking_profile()
	profile_enabled = OS.get_environment("OPENVT_PROFILE") == "1"
	var profile_report_frames_text := OS.get_environment("OPENVT_PROFILE_REPORT_FRAMES")
	if not profile_report_frames_text.is_empty():
		if not profile_report_frames_text.is_valid_int() or profile_report_frames_text.to_int() <= 0:
			_fail(2, "OPENVT_PROFILE_REPORT_FRAMES must be a positive integer")
			return
		profile_report_frames = profile_report_frames_text.to_int()

	if not ClassDB.class_exists("OpenVTDmaBufCapture"):
		_fail(2, "OpenVTDmaBufCapture module is unavailable")
		return

	var rtsp_url := OS.get_environment("OPENVT_RTSP_URL")
	if rtsp_url.is_empty():
		_fail(2, "OPENVT_RTSP_URL is not set")
		return
	var total_frames_text := OS.get_environment("OPENVT_LIVE2D_TOTAL_FRAMES")
	if not total_frames_text.is_empty():
		if not total_frames_text.is_valid_int() or total_frames_text.to_int() < 0:
			_fail(2, "OPENVT_LIVE2D_TOTAL_FRAMES must be a non-negative integer")
			return
		total_frames = total_frames_text.to_int()

	dma_capture = ClassDB.instantiate("OpenVTDmaBufCapture")
	var configure_error: int = dma_capture.configure(
		rtsp_url,
		OUTPUT_SIZE.x,
		OUTPUT_SIZE.y,
		OUTPUT_FPS,
		total_frames
	)
	if configure_error != OK:
		_fail(3, "DMA-BUF capture configure failed: error=%d detail=%s" % [
			configure_error,
			dma_capture.get_last_error(),
		])
		return

	render_viewport = SubViewport.new()
	render_viewport.name = "Live2DDmaBufViewport"
	render_viewport.size = OUTPUT_SIZE
	render_viewport.disable_3d = true
	render_viewport.transparent_bg = false
	render_viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child(render_viewport)

	model = AyagamiLoader.load_model(MODEL_PATH)
	if model == null:
		_fail(4, "model load failed: %s" % MODEL_PATH)
		return

	render_viewport.add_child(model)
	for parameter in model.get_parameters():
		available_parameters[parameter] = true
	var scale_factor := minf(
		(float(OUTPUT_SIZE.x) * 0.92) / float(model.size.x),
		(float(OUTPUT_SIZE.y) * 0.92) / float(model.size.y)
	)
	base_model_scale = scale_factor
	_apply_view_transform()

	motion_controller = model.get_node_or_null("MotionController") as AnimationPlayer
	if motion_controller == null:
		_fail(5, "model has no MotionController")
		return

	var motion_library := AyagamiLoader.load_motion_library(model)
	if motion_library == null:
		_fail(6, "motion library load failed")
		return

	if motion_controller.has_animation_library(""):
		motion_controller.remove_animation_library("")
	motion_controller.add_animation_library("", motion_library)
	var tracking_port_text := OS.get_environment("OPENVT_LIVE2D_TRACKING_PORT")
	if tracking_port_text.is_empty() and not motion_controller.has_animation(SHAKE_MOTION):
		_fail(7, "missing motion: %s available=%s" % [
			SHAKE_MOTION,
			motion_library.get_animation_list(),
		])
		return

	if not tracking_port_text.is_empty():
		if not tracking_port_text.is_valid_int():
			_fail(10, "OPENVT_LIVE2D_TRACKING_PORT is not an integer")
			return
		var tracking_port := tracking_port_text.to_int()
		var tracking_address := OS.get_environment("OPENVT_LIVE2D_TRACKING_ADDRESS")
		if tracking_address.is_empty():
			tracking_address = "127.0.0.1"
		tracking_receiver = Live2DTrackingReceiver.new()
		tracking_receiver.set_profile_enabled(profile_enabled)
		var tracking_error: Error = tracking_receiver.open(tracking_address, tracking_port)
		if tracking_error != OK:
			_fail(10, "Live2D tracking UDP bind failed: address=%s port=%d error=%d" % [
				tracking_address,
				tracking_port,
				tracking_error,
			])
			return
		tracking_enabled = true
		print("LIVE2D_TRACKING_LISTEN address=%s port=%d max_age_ms=%.1f" % [
			tracking_address,
			tracking_port,
			float(TRACKING_MAX_AGE_USEC) / 1000.0,
		])

	_set_default_face_parameters()
	if profile_enabled and model.has_method("profile_reset"):
		model.call("profile_reset")
	print("VIDEO_CONFIG path=Godot_SubViewport_to_DMA_BUF size=%s fps=%d frames=%d model_size=%s scale=%.6f meshes=%d parameters=%d motion=%s tracking=%s" % [
		OUTPUT_SIZE,
		OUTPUT_FPS,
		total_frames,
		model.size,
		scale_factor,
		model.get_node("Meshes").get_child_count(),
		model.get_parameters().size(),
		SHAKE_MOTION,
		tracking_enabled,
	])
	print("VIDEO_MOTIONS available=%s" % [motion_library.get_animation_list()])


func _process(delta: float) -> void:
	if finishing or capture_pending:
		return

	if capture_start_usec == 0:
		capture_start_usec = Time.get_ticks_usec()

	var next_frame := frame_index + 1
	var animation_started_usec := Time.get_ticks_usec()
	if tracking_enabled:
		_apply_live_tracking(delta, next_frame)
	else:
		_apply_frame_animation(next_frame)
		if next_frame == MOTION_START_FRAME:
			_start_shake_motion()
	if profile_enabled:
		profile_animation_usec += Time.get_ticks_usec() - animation_started_usec

	capture_pending = true
	set_process(false)
	if profile_enabled:
		profile_draw_wait_started_usec = Time.get_ticks_usec()
	_capture_frame_after_draw(next_frame)


func _apply_frame_animation(frame: int) -> void:
	if frame < 60:
		_set_default_face_parameters()
		return

	if frame < 90:
		var blink_progress := float(frame - 60) / 30.0
		var eye_open := 1.0 - sin(blink_progress * PI) * sin(blink_progress * PI)
		_set_parameter_if_present("ParamEyeLOpen", eye_open)
		_set_parameter_if_present("ParamEyeROpen", eye_open)
		return

	if frame < 150:
		var turn_progress := float(frame - 90) / 60.0
		_set_parameter_if_present("ParamAngleX", sin(turn_progress * TAU) * 25.0)
		_set_parameter_if_present("ParamAngleY", sin(turn_progress * PI) * 12.0)
		_set_parameter_if_present("ParamBodyAngleX", sin(turn_progress * TAU) * 6.0)
		return

	if frame < MOTION_START_FRAME:
		var mouth_progress := float(frame - 150) / 60.0
		var mouth_open := (sin(mouth_progress * TAU * 2.0) + 1.0) * 0.4
		_set_parameter_if_present("ParamMouthOpenY", mouth_open)
		_set_parameter_if_present("ParamMouthForm", sin(mouth_progress * TAU) * 0.5)


func _apply_live_tracking(delta: float, render_frame: int) -> void:
	var packet: Dictionary = tracking_receiver.receive_latest()
	var applied_new_source := false
	if not packet.is_empty():
		var points_ready_usec := int(int(packet["points_ready_ns"]) / 1000)
		var points_ready_local_usec := points_ready_usec + int(packet["source_clock_offset_usec"])
		var packet_age_usec := Time.get_ticks_usec() - points_ready_local_usec
		if profile_enabled:
			if packet_age_usec >= 0:
				_record_tracking_sample("packet_age", packet_age_usec)
			else:
				_record_tracking_count("future_packets", 1)
		if packet_age_usec >= 0 and packet_age_usec <= TRACKING_MAX_AGE_USEC:
			var apply_started_usec := Time.get_ticks_usec()
			tracking_last_packet_usec = apply_started_usec
			if packet.has("view"):
				_apply_view_state(packet["view"] as Dictionary)
			_apply_tracking_parameters(packet["parameters"] as Dictionary)
			var apply_finished_usec := Time.get_ticks_usec()
			if profile_enabled:
				_record_tracking_sample("parameter_apply", apply_finished_usec - apply_started_usec)
				_record_tracking_sample(
					"receive_to_apply",
					apply_started_usec - int(packet["received_at_usec"])
				)
			var source_key := "%d:%d" % [
				int(packet.get("session_id", 0)),
				int(packet.get("frame_id", 0)),
			]
			if source_key != tracking_active_source_key:
				tracking_active_source_key = source_key
				applied_new_source = true
				if profile_enabled:
					_record_tracking_count("source_frames", 1)
			tracking_last_source_frame = int(packet.get("frame_id", 0))
			var calibrated := bool(packet.get("calibrated", false))
			if calibrated != tracking_last_calibrated:
				tracking_last_calibrated = calibrated
				print("LIVE2D_TRACKING_CALIBRATED value=%s frame_id=%d" % [
					calibrated,
					int(packet.get("frame_id", 0)),
				])
		else:
			if profile_enabled:
				_record_tracking_count("stale_packets", 1)

	var active_source_available := tracking_last_packet_usec > 0
	if active_source_available:
		active_source_available = (
			Time.get_ticks_usec() - tracking_last_packet_usec <= TRACKING_MAX_AGE_USEC
		)
	if profile_enabled:
		_record_tracking_count("render_frames", 1)
		if active_source_available:
			_record_tracking_count("associated_frames", 1)
			tracking_last_associated_render_frame = render_frame
			if not applied_new_source:
				_record_tracking_count("reused_source_frames", 1)
		else:
			_record_tracking_count("no_source_frames", 1)
	if active_source_available:
		return

	if tracking_last_packet_usec == 0:
		_relax_face_parameters(delta)
		return
	if Time.get_ticks_usec() - tracking_last_packet_usec > TRACKING_MAX_AGE_USEC:
		tracking_active_source_key = ""
		_relax_face_parameters(delta)


func _apply_tracking_parameters(parameters: Dictionary) -> void:
	for parameter in TRACKED_FACE_PARAMETERS:
		var parameter_text := String(parameter)
		if parameters.has(parameter_text):
			_set_parameter_if_present(parameter, float(parameters[parameter_text]))


func _apply_view_state(view: Dictionary) -> void:
	view_zoom = clampf(float(view.get("zoom", 1.0)), VIEW_ZOOM_MIN, VIEW_ZOOM_MAX)
	view_offset = Vector2(
		clampf(float(view.get("offset_x", 0.0)), VIEW_OFFSET_MIN, VIEW_OFFSET_MAX),
		clampf(float(view.get("offset_y", 0.0)), VIEW_OFFSET_MIN, VIEW_OFFSET_MAX)
	)
	_apply_view_transform()


func _apply_view_transform() -> void:
	if model == null:
		return
	model.scale = Vector2.ONE * base_model_scale * view_zoom
	model.position = Vector2(OUTPUT_SIZE) * (Vector2(0.5, 0.5) + view_offset)


func _empty_tracking_profile() -> Dictionary:
	return {
		"render_frames": 0,
		"associated_frames": 0,
		"source_frames": 0,
		"reused_source_frames": 0,
		"no_source_frames": 0,
		"stale_packets": 0,
		"future_packets": 0,
		"packet_age_samples": 0,
		"packet_age_usec": 0,
		"packet_age_max_usec": 0,
		"parameter_apply_samples": 0,
		"parameter_apply_usec": 0,
		"parameter_apply_max_usec": 0,
		"receive_to_apply_samples": 0,
		"receive_to_apply_usec": 0,
		"receive_to_apply_max_usec": 0,
	}


func _record_tracking_count(name: String, value: int) -> void:
	tracking_profile_total[name] = int(tracking_profile_total[name]) + value
	tracking_profile_window[name] = int(tracking_profile_window[name]) + value


func _record_tracking_sample(name: String, duration_usec: int) -> void:
	if duration_usec < 0:
		return
	var samples_name := "%s_samples" % name
	var total_name := "%s_usec" % name
	var maximum_name := "%s_max_usec" % name
	_record_tracking_count(samples_name, 1)
	_record_tracking_count(total_name, duration_usec)
	if duration_usec > int(tracking_profile_total[maximum_name]):
		tracking_profile_total[maximum_name] = duration_usec
	if duration_usec > int(tracking_profile_window[maximum_name]):
		tracking_profile_window[maximum_name] = duration_usec


func _average_milliseconds(total_usec: int, samples: int) -> float:
	if samples <= 0:
		return 0.0
	return float(total_usec) / float(samples) / 1000.0


func _print_tracking_profile(final_report: bool) -> void:
	if not profile_enabled or not tracking_enabled or tracking_receiver == null:
		return
	if final_report and tracking_profile_final_printed:
		return

	var label := "WINDOW"
	var local_stats: Dictionary = tracking_profile_window
	var receiver_stats: Dictionary = tracking_receiver.take_profile_window()
	if final_report:
		label = "FINAL"
		local_stats = tracking_profile_total
		receiver_stats = tracking_receiver.profile_totals()
		tracking_profile_final_printed = true

	print("LIVE2D_TRACKING_PROFILE_%s render_frames=%d source_frames=%d associated_frames=%d reused_source_frames=%d no_source_frames=%d received=%d valid=%d accepted=%d invalid=%d out_of_order=%d overwritten=%d stale=%d future=%d last_source_frame=%d last_render_frame=%d" % [
		label,
		int(local_stats["render_frames"]),
		int(local_stats["source_frames"]),
		int(local_stats["associated_frames"]),
		int(local_stats["reused_source_frames"]),
		int(local_stats["no_source_frames"]),
		int(receiver_stats["received"]),
		int(receiver_stats["valid"]),
		int(receiver_stats["accepted"]),
		int(receiver_stats["invalid"]),
		int(receiver_stats["out_of_order"]),
		int(receiver_stats["overwritten"]),
		int(local_stats["stale_packets"]),
		int(local_stats["future_packets"]),
		tracking_last_source_frame,
		tracking_last_associated_render_frame,
	])
	print("LIVE2D_TRACKING_LATENCY_%s receive_samples=%d receive_avg_ms=%.6f parse_avg_ms=%.6f packet_age_samples=%d packet_age_avg_ms=%.6f packet_age_max_ms=%.6f receive_to_apply_samples=%d receive_to_apply_avg_ms=%.6f receive_to_apply_max_ms=%.6f parameter_apply_samples=%d parameter_apply_avg_ms=%.6f parameter_apply_max_ms=%.6f" % [
		label,
		int(receiver_stats["received"]),
		_average_milliseconds(
			int(receiver_stats["receive_usec"]),
			int(receiver_stats["received"])
		),
		_average_milliseconds(
			int(receiver_stats["parse_usec"]),
			int(receiver_stats["received"])
		),
		int(local_stats["packet_age_samples"]),
		_average_milliseconds(
			int(local_stats["packet_age_usec"]),
			int(local_stats["packet_age_samples"])
		),
		float(local_stats["packet_age_max_usec"]) / 1000.0,
		int(local_stats["receive_to_apply_samples"]),
		_average_milliseconds(
			int(local_stats["receive_to_apply_usec"]),
			int(local_stats["receive_to_apply_samples"])
		),
		float(local_stats["receive_to_apply_max_usec"]) / 1000.0,
		int(local_stats["parameter_apply_samples"]),
		_average_milliseconds(
			int(local_stats["parameter_apply_usec"]),
			int(local_stats["parameter_apply_samples"])
		),
		float(local_stats["parameter_apply_max_usec"]) / 1000.0,
	])

	if not final_report:
		tracking_profile_window = _empty_tracking_profile()


func _relax_face_parameters(delta: float) -> void:
	var alpha := 1.0 - exp(-delta / TRACKING_FALLBACK_TAU_SECONDS)
	for parameter in TRACKED_FACE_PARAMETERS:
		if not available_parameters.has(parameter):
			continue
		var property_path := "parameters/%s" % parameter
		var default_value := float(model.get("%s/default" % property_path))
		var current_value := float(model.get(property_path))
		var next_value := current_value + alpha * (default_value - current_value)
		_set_parameter_if_present(parameter, next_value)


func _start_shake_motion() -> void:
	_set_default_face_parameters()
	motion_controller.play("RESET")
	motion_controller.advance(0.001)
	motion_controller.play(SHAKE_MOTION)
	print("VIDEO_MOTION_STARTED frame=%d motion=%s" % [MOTION_START_FRAME, SHAKE_MOTION])


func _set_default_face_parameters() -> void:
	_set_parameter_if_present("ParamAngleX", 0.0)
	_set_parameter_if_present("ParamAngleY", 0.0)
	_set_parameter_if_present("ParamAngleZ", 0.0)
	_set_parameter_if_present("ParamBodyAngleX", 0.0)
	_set_parameter_if_present("ParamBodyAngleY", 0.0)
	_set_parameter_if_present("ParamBodyAngleZ", 0.0)
	_set_parameter_if_present("ParamMouthOpenY", 0.0)
	_set_parameter_if_present("ParamMouthForm", 0.0)
	_set_parameter_if_present("ParamEyeLOpen", 1.0)
	_set_parameter_if_present("ParamEyeROpen", 1.0)


func _set_parameter_if_present(parameter: StringName, value: float) -> void:
	if not available_parameters.has(parameter):
		return
	var property_path := "parameters/%s" % parameter
	var parameter_range: Vector2 = model.get("%s/range" % property_path)
	model.set(property_path, clampf(value, parameter_range.x, parameter_range.y))


func _capture_frame_after_draw(captured_frame: int) -> void:
	await RenderingServer.frame_post_draw
	if profile_enabled:
		profile_draw_wait_usec += Time.get_ticks_usec() - profile_draw_wait_started_usec
	var end_of_stream := total_frames > 0 and captured_frame == total_frames
	var viewport_rid := render_viewport.get_viewport_rid()
	var capture_started_usec := Time.get_ticks_usec()
	var capture_error: int = dma_capture.capture_frame(viewport_rid, captured_frame - 1, end_of_stream)
	if profile_enabled:
		profile_capture_usec += Time.get_ticks_usec() - capture_started_usec
	if capture_error != OK:
		_fail(8, "DMA-BUF frame capture failed: frame=%d error=%d detail=%s" % [
			captured_frame,
			capture_error,
			dma_capture.get_last_error(),
		])
		return

	frame_index = captured_frame
	if frame_index == 1 or frame_index % OUTPUT_FPS == 0:
		print("VIDEO_FRAME_CAPTURED frame=%d viewport_rid=%s" % [frame_index, viewport_rid])
	if (
		profile_enabled
		and tracking_enabled
		and total_frames == 0
		and frame_index % profile_report_frames == 0
	):
		_print_tracking_profile(false)

	if total_frames == 0 or frame_index < total_frames:
		capture_pending = false
		set_process(true)
		return

	finishing = true
	var finish_error: int = dma_capture.finish()
	if finish_error != OK:
		_fail(9, "DMA-BUF capture finish failed: error=%d detail=%s" % [
			finish_error,
			dma_capture.get_last_error(),
		])
		return

	var elapsed_usec := Time.get_ticks_usec() - capture_start_usec
	var elapsed_sec := float(elapsed_usec) / 1000000.0
	print("VIDEO_RESULT frames=%d media_seconds=%.3f wall_seconds=%.6f throughput_fps=%.3f" % [
		total_frames,
		float(total_frames) / float(OUTPUT_FPS),
		elapsed_sec,
		float(total_frames) / elapsed_sec,
	])
	if profile_enabled:
		if model.has_method("profile_report"):
			model.call("profile_report")
		print("SCRIPT_PROFILE_RESULT frames=%d animation_ms=%.6f draw_wait_ms=%.6f capture_call_ms=%.6f" % [
			total_frames,
			float(profile_animation_usec) / 1000.0 / float(total_frames),
			float(profile_draw_wait_usec) / 1000.0 / float(total_frames),
			float(profile_capture_usec) / 1000.0 / float(total_frames),
		])
		_print_tracking_profile(true)
	if tracking_receiver != null:
		tracking_receiver.close()
	get_tree().quit()


func _fail(exit_code: int, message: String) -> void:
	push_error("VIDEO_ERROR %s" % message)
	_print_tracking_profile(true)
	if tracking_receiver != null:
		tracking_receiver.close()
	if dma_capture != null:
		dma_capture.finish()
	get_tree().quit(exit_code)


func _exit_tree() -> void:
	_print_tracking_profile(true)

