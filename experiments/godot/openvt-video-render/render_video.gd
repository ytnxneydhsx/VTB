extends Node2D

const MODEL_PATH := "/home/huangnv/open-vt-model/阿芙洛狄忒模型文件/fense_vts/fense.model3.json"
const OUTPUT_SIZE := Vector2i(1920, 1080)
const OUTPUT_FPS := 30
const TOTAL_FRAMES := 300
const MOTION_START_FRAME := 210
const SHAKE_MOTION := &"yaotou.motion3.json"

var model: AyagamiModel
var motion_controller: AnimationPlayer
var render_viewport: SubViewport
var frame_index := 0
var capture_start_usec := 0
var finishing := false
var capture_pending := false
var frames_dir := ""


func _ready() -> void:
	Engine.max_fps = 0
	RenderingServer.set_default_clear_color(Color(0.18, 0.18, 0.18, 1.0))

	var task_output_dir := OS.get_environment("OPENVT_RENDER_OUTPUT_DIR")
	if task_output_dir.is_empty():
		push_error("VIDEO_ERROR OPENVT_RENDER_OUTPUT_DIR is not set")
		get_tree().quit(2)
		return
	frames_dir = task_output_dir.path_join("frames")
	if not DirAccess.dir_exists_absolute(frames_dir):
		push_error("VIDEO_ERROR frames directory is missing: %s" % frames_dir)
		get_tree().quit(2)
		return

	render_viewport = SubViewport.new()
	render_viewport.name = "Live2DVideoViewport"
	render_viewport.size = OUTPUT_SIZE
	render_viewport.disable_3d = true
	render_viewport.transparent_bg = false
	render_viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child(render_viewport)

	model = AyagamiLoader.load_model(MODEL_PATH)
	if model == null:
		push_error("VIDEO_ERROR model load failed: %s" % MODEL_PATH)
		get_tree().quit(3)
		return

	render_viewport.add_child(model)
	var scale_factor := minf(
		(float(OUTPUT_SIZE.x) * 0.92) / float(model.size.x),
		(float(OUTPUT_SIZE.y) * 0.92) / float(model.size.y)
	)
	model.scale = Vector2.ONE * scale_factor
	model.position = Vector2(OUTPUT_SIZE) * 0.5

	motion_controller = model.get_node_or_null("MotionController") as AnimationPlayer
	if motion_controller == null:
		push_error("VIDEO_ERROR model has no MotionController")
		get_tree().quit(4)
		return

	var motion_library := AyagamiLoader.load_motion_library(model)
	if motion_library == null:
		push_error("VIDEO_ERROR motion library load failed")
		get_tree().quit(5)
		return

	if motion_controller.has_animation_library(""):
		motion_controller.remove_animation_library("")
	motion_controller.add_animation_library("", motion_library)

	if not motion_controller.has_animation(SHAKE_MOTION):
		push_error("VIDEO_ERROR missing motion: %s available=%s" % [
			SHAKE_MOTION,
			motion_library.get_animation_list(),
		])
		get_tree().quit(6)
		return

	_set_default_face_parameters()
	print("VIDEO_CONFIG size=%s fps=%d frames=%d model_size=%s scale=%.6f meshes=%d parameters=%d motion=%s" % [
		OUTPUT_SIZE,
		OUTPUT_FPS,
		TOTAL_FRAMES,
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
	_apply_frame_animation(next_frame)

	if next_frame == MOTION_START_FRAME:
		_start_shake_motion()

	capture_pending = true
	set_process(false)
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
		return


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
	_set_parameter_if_present("ParamMouthOpenY", 0.0)
	_set_parameter_if_present("ParamMouthForm", 0.0)
	_set_parameter_if_present("ParamEyeLOpen", 1.0)
	_set_parameter_if_present("ParamEyeROpen", 1.0)


func _set_parameter_if_present(parameter: StringName, value: float) -> void:
	if parameter in model.get_parameters():
		model.set("parameters/%s" % parameter, value)


func _capture_frame_after_draw(captured_frame: int) -> void:
	await RenderingServer.frame_post_draw
	var image := render_viewport.get_texture().get_image()
	var frame_path := frames_dir.path_join("frame-%04d.jpg" % captured_frame)
	var save_error := image.save_jpg(frame_path, 0.92)
	if save_error != OK:
		push_error("VIDEO_ERROR frame save failed: frame=%d error=%d path=%s" % [
			captured_frame,
			save_error,
			frame_path,
		])
		get_tree().quit(7)
		return

	frame_index = captured_frame
	if frame_index == 1 or frame_index % OUTPUT_FPS == 0:
		print("VIDEO_FRAME_SAVED frame=%d path=%s" % [frame_index, frame_path])

	if frame_index < TOTAL_FRAMES:
		capture_pending = false
		set_process(true)
		return

	finishing = true
	var elapsed_usec := Time.get_ticks_usec() - capture_start_usec
	var elapsed_sec := float(elapsed_usec) / 1000000.0
	var throughput_fps := float(TOTAL_FRAMES) / elapsed_sec
	print("VIDEO_RESULT frames=%d media_seconds=%.3f wall_seconds=%.6f throughput_fps=%.3f" % [
		TOTAL_FRAMES,
		float(TOTAL_FRAMES) / float(OUTPUT_FPS),
		elapsed_sec,
		throughput_fps,
	])
	get_tree().quit()
