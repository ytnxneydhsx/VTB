extends Node2D

const MODEL_PATH := "/home/huangnv/open-vt-model/阿芙洛狄忒模型文件/fense_vts/fense.model3.json"
const OUTPUT_SIZE := Vector2i(1920, 1080)
const OUTPUT_FPS := 30
const DEFAULT_TOTAL_FRAMES := 300
const MOTION_START_FRAME := 210
const SHAKE_MOTION := &"yaotou.motion3.json"

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
var total_frames := DEFAULT_TOTAL_FRAMES
var base_model_scale := 1.0
var view_zoom := 1.0
var view_offset := Vector2.ZERO


func _ready() -> void:
	Engine.max_fps = OUTPUT_FPS
	RenderingServer.set_default_clear_color(Color(0.18, 0.18, 0.18, 1.0))
	profile_enabled = OS.get_environment("OPENVT_PROFILE") == "1"
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
	if not motion_controller.has_animation(SHAKE_MOTION):
		_fail(7, "missing motion: %s available=%s" % [
			SHAKE_MOTION,
			motion_library.get_animation_list(),
		])
		return

	_set_default_face_parameters()
	if profile_enabled and model.has_method("profile_reset"):
		model.call("profile_reset")
	print("VIDEO_CONFIG path=Godot_SubViewport_to_DMA_BUF size=%s fps=%d frames=%d model_size=%s scale=%.6f meshes=%d parameters=%d motion=%s" % [
		OUTPUT_SIZE,
		OUTPUT_FPS,
		total_frames,
		model.size,
		scale_factor,
		model.get_node("Meshes").get_child_count(),
		model.get_parameters().size(),
		SHAKE_MOTION,
	])
	print("VIDEO_MOTIONS available=%s" % [motion_library.get_animation_list()])


func _process(_delta: float) -> void:
	if finishing or capture_pending:
		return

	if capture_start_usec == 0:
		capture_start_usec = Time.get_ticks_usec()

	var next_frame := frame_index + 1
	var animation_started_usec := Time.get_ticks_usec()
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


func _apply_view_transform() -> void:
	if model == null:
		return
	model.scale = Vector2.ONE * base_model_scale * view_zoom
	model.position = Vector2(OUTPUT_SIZE) * (Vector2(0.5, 0.5) + view_offset)


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
	get_tree().quit()


func _fail(exit_code: int, message: String) -> void:
	push_error("VIDEO_ERROR %s" % message)
	if dma_capture != null:
		dma_capture.finish()
	get_tree().quit(exit_code)


func _exit_tree() -> void:
	pass

