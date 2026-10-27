extends Node

const MODEL_PATH := "/home/huangnv/open-vt-model/阿芙洛狄忒模型文件/fense_vts/fense.model3.json"
const OUTPUT_PATH := "/tmp/openvt-offscreen-1920x1080.png"
const OUTPUT_SIZE := Vector2i(1920, 1080)
const WARMUP_FRAMES := 180
const SAMPLE_FRAMES := 600

var render_viewport: SubViewport
var model: AyagamiModel
var frame_index := 0
var sample_start_usec := 0


func _ready() -> void:
	Engine.max_fps = 0
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)

	render_viewport = SubViewport.new()
	render_viewport.name = "Live2DOffscreenViewport"
	render_viewport.size = OUTPUT_SIZE
	render_viewport.disable_3d = true
	render_viewport.transparent_bg = false
	render_viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child(render_viewport)

	model = AyagamiLoader.load_model(MODEL_PATH)
	if model == null:
		push_error("OFFSCREEN_ERROR model load failed: %s" % MODEL_PATH)
		get_tree().quit(2)
		return
	render_viewport.add_child(model)

	await get_tree().process_frame
	var scale_factor: float = minf(
		(float(OUTPUT_SIZE.x) * 0.92) / float(model.size.x),
		(float(OUTPUT_SIZE.y) * 0.92) / float(model.size.y)
	)
	model.scale = Vector2.ONE * scale_factor
	model.position = Vector2(OUTPUT_SIZE) * 0.5
	print("OFFSCREEN_MODEL output=%s model_size=%s scale=%.6f meshes=%d parameters=%d" % [
		OUTPUT_SIZE,
		model.size,
		scale_factor,
		model.get_node("Meshes").get_child_count(),
		model.get_parameters().size(),
	])


func _process(_delta: float) -> void:
	frame_index += 1
	var phase := float(frame_index) * 0.035
	_set_parameter_if_present("ParamAngleX", sin(phase) * 25.0)
	_set_parameter_if_present("ParamAngleY", cos(phase * 0.83) * 18.0)
	_set_parameter_if_present("ParamBodyAngleX", sin(phase * 0.57) * 8.0)
	_set_parameter_if_present("ParamMouthOpenY", (sin(phase * 1.7) + 1.0) * 0.35)
	_set_parameter_if_present("ParamEyeLOpen", 0.65 + sin(phase * 0.4) * 0.30)
	_set_parameter_if_present("ParamEyeROpen", 0.65 + sin(phase * 0.4) * 0.30)

	if frame_index == WARMUP_FRAMES:
		sample_start_usec = Time.get_ticks_usec()
		var image := render_viewport.get_texture().get_image()
		var save_error := image.save_png(OUTPUT_PATH)
		print("OFFSCREEN_SCREENSHOT error=%d path=%s size=%s" % [
			save_error,
			OUTPUT_PATH,
			image.get_size(),
		])

	if frame_index == WARMUP_FRAMES + SAMPLE_FRAMES:
		var elapsed_usec := Time.get_ticks_usec() - sample_start_usec
		var elapsed_sec := float(elapsed_usec) / 1000000.0
		var fps := float(SAMPLE_FRAMES) / elapsed_sec
		print("OFFSCREEN_RESULT frames=%d seconds=%.6f fps=%.3f ms_per_frame=%.3f" % [
			SAMPLE_FRAMES,
			elapsed_sec,
			fps,
			1000.0 / fps,
		])
		get_tree().quit()


func _set_parameter_if_present(parameter: StringName, value: float) -> void:
	if parameter in model.get_parameters():
		model.set("parameters/%s" % parameter, value)
