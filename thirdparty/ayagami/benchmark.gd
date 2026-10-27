extends Node2D

const MODEL_PATH := "/home/huangnv/open-vt-model/阿芙洛狄忒模型文件/fense_vts/fense.model3.json"
const WARMUP_FRAMES := 180
const SAMPLE_FRAMES := 600

var model: AyagamiModel
var frame_index := 0
var sample_start_usec := 0
var sample_cpu_start_usec := 0

func _ready() -> void:
	Engine.max_fps = 0
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	model = AyagamiLoader.load_model(MODEL_PATH)
	if model == null:
		push_error("BENCHMARK_ERROR model load failed: %s" % MODEL_PATH)
		get_tree().quit(2)
		return
	add_child(model)
	await get_tree().process_frame
	var viewport_size := get_viewport_rect().size
	var scale_factor: float = minf(
		(viewport_size.x * 0.92) / float(model.size.x),
		(viewport_size.y * 0.92) / float(model.size.y)
	)
	model.scale = Vector2.ONE * scale_factor
	model.position = viewport_size * 0.5
	print("BENCHMARK_MODEL path=%s size=%s origin=%s scale=%.6f meshes=%d parameters=%d" % [
		MODEL_PATH,
		model.size,
		model.origin,
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
		sample_cpu_start_usec = Time.get_ticks_usec()
		var image := get_viewport().get_texture().get_image()
		var screenshot_error := image.save_png("/tmp/openvt-benchmark.png")
		print("BENCHMARK_SCREENSHOT error=%d path=/tmp/openvt-benchmark.png" % screenshot_error)

	if frame_index == WARMUP_FRAMES + SAMPLE_FRAMES:
		var elapsed_usec := Time.get_ticks_usec() - sample_start_usec
		var elapsed_sec := float(elapsed_usec) / 1000000.0
		var fps := float(SAMPLE_FRAMES) / elapsed_sec
		print("BENCHMARK_RESULT frames=%d seconds=%.6f fps=%.3f ms_per_frame=%.3f" % [
			SAMPLE_FRAMES,
			elapsed_sec,
			fps,
			1000.0 / fps,
		])
		get_tree().quit()

func _set_parameter_if_present(parameter: StringName, value: float) -> void:
	if parameter in model.get_parameters():
		model.set("parameters/%s" % parameter, value)
