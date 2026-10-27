extends Node

const MODEL_PATH := "/home/huangnv/open-vt-model/阿芙洛狄忒模型文件/fense_vts/fense.model3.json"
const OUTPUT_SIZE := Vector2i(1920, 1080)
const WARMUP_FRAMES := 180
const SAMPLE_FRAMES := 600

var render_viewport: SubViewport
var model: AyagamiModel
var frame_index := 0
var sample_start_usec := 0
var parameter := "ParamMouthOpenY"


func _ready() -> void:
	Engine.max_fps = 0
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	parameter = OS.get_environment("OPENVT_BENCHMARK_PARAMETER")
	if parameter.is_empty():
		parameter = "ParamMouthOpenY"

	render_viewport = SubViewport.new()
	render_viewport.size = OUTPUT_SIZE
	render_viewport.disable_3d = true
	render_viewport.transparent_bg = false
	render_viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child(render_viewport)

	model = AyagamiLoader.load_model(MODEL_PATH)
	if model == null:
		push_error("UPDATED_MESH_LIST_ERROR model load failed: %s" % MODEL_PATH)
		get_tree().quit(2)
		return
	render_viewport.add_child(model)

	await get_tree().process_frame
	if parameter not in model.get_parameters():
		push_error("UPDATED_MESH_LIST_ERROR parameter missing: %s" % parameter)
		get_tree().quit(3)
		return
	var scale_factor: float = minf(
		(float(OUTPUT_SIZE.x) * 0.92) / float(model.size.x),
		(float(OUTPUT_SIZE.y) * 0.92) / float(model.size.y)
	)
	model.scale = Vector2.ONE * scale_factor
	model.position = Vector2(OUTPUT_SIZE) * 0.5
	print("UPDATED_MESH_LIST_MODEL parameter=%s meshes=%d" % [
		parameter,
		model.get_node("Meshes").get_child_count(),
	])


func _process(_delta: float) -> void:
	frame_index += 1
	var phase := float(frame_index) * 0.035
	var value := _parameter_value(parameter, phase)
	model.set("parameters/%s" % parameter, value)

	if frame_index == WARMUP_FRAMES:
		sample_start_usec = Time.get_ticks_usec()

	if frame_index == WARMUP_FRAMES + SAMPLE_FRAMES:
		var elapsed_usec := Time.get_ticks_usec() - sample_start_usec
		var elapsed_ms := float(elapsed_usec) / 1000.0
		print("UPDATED_MESH_LIST_RESULT parameter=%s frames=%d ms_per_frame=%.6f fps=%.3f" % [
			parameter,
			SAMPLE_FRAMES,
			elapsed_ms / float(SAMPLE_FRAMES),
			float(SAMPLE_FRAMES) * 1000.0 / elapsed_ms,
		])
		var output_path := OS.get_environment("OPENVT_BENCHMARK_OUTPUT_PNG")
		if not output_path.is_empty():
			var image := render_viewport.get_texture().get_image()
			var save_error := image.save_png(output_path)
			if save_error != OK:
				push_error("UPDATED_MESH_LIST_ERROR save failed: %s (%d)" % [output_path, save_error])
				get_tree().quit(4)
				return
			print("UPDATED_MESH_LIST_IMAGE path=%s" % output_path)
		get_tree().quit()


func _parameter_value(parameter_name: String, phase: float) -> float:
	if parameter_name.contains("Eye") and parameter_name.contains("Open"):
		return 0.65 + sin(phase) * 0.30
	if parameter_name.contains("MouthOpen"):
		return (sin(phase) + 1.0) * 0.35
	if parameter_name.contains("Angle"):
		return sin(phase) * 25.0
	return sin(phase)
