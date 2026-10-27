extends Node

const MODEL_PATH := "/home/huangnv/open-vt-model/阿芙洛狄忒模型文件/fense_vts/fense.model3.json"
const OUTPUT_SIZE := Vector2i(1920, 1080)
const WARMUP_FRAMES := 180
const SAMPLE_FRAMES := 600

var render_viewport: SubViewport
var model: AyagamiModel
var frame_index := 0
var sample_start_usec := 0
var frame_setup_cpu_ms := 0.0
var viewport_cpu_ms := 0.0
var root_viewport_cpu_ms := 0.0
var viewport_gpu_ms := 0.0
var root_viewport_gpu_ms := 0.0
var viewport_gpu_valid_samples := 0
var viewport_gpu_invalid_samples := 0
var draw_calls := 0


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

	RenderingServer.viewport_set_measure_render_time(
		render_viewport.get_viewport_rid(),
		true,
	)
	RenderingServer.viewport_set_measure_render_time(
		get_viewport().get_viewport_rid(),
		true,
	)

	model = AyagamiLoader.load_model(MODEL_PATH)
	if model == null:
		push_error("STAGE_PROFILE_ERROR model load failed: %s" % MODEL_PATH)
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
	print("STAGE_PROFILE_MODEL output=%s meshes=%d parameters=%d" % [
		OUTPUT_SIZE,
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

	if frame_index > WARMUP_FRAMES and frame_index <= WARMUP_FRAMES + SAMPLE_FRAMES:
		_collect_render_metrics()

	if frame_index == WARMUP_FRAMES + SAMPLE_FRAMES:
		_print_results()
		get_tree().quit()


func _collect_render_metrics() -> void:
	var render_rid := render_viewport.get_viewport_rid()
	var root_rid := get_viewport().get_viewport_rid()
	frame_setup_cpu_ms += RenderingServer.get_frame_setup_time_cpu()
	viewport_cpu_ms += RenderingServer.viewport_get_measured_render_time_cpu(render_rid)
	root_viewport_cpu_ms += RenderingServer.viewport_get_measured_render_time_cpu(root_rid)

	var gpu_ms := RenderingServer.viewport_get_measured_render_time_gpu(render_rid)
	var root_gpu_ms := RenderingServer.viewport_get_measured_render_time_gpu(root_rid)
	if is_finite(gpu_ms) and is_finite(root_gpu_ms) and gpu_ms >= 0.0 and root_gpu_ms >= 0.0 and gpu_ms < 1000.0 and root_gpu_ms < 1000.0:
		viewport_gpu_ms += gpu_ms
		root_viewport_gpu_ms += root_gpu_ms
		viewport_gpu_valid_samples += 1
	else:
		viewport_gpu_invalid_samples += 1

	draw_calls += RenderingServer.viewport_get_render_info(
		render_rid,
		RenderingServer.VIEWPORT_RENDER_INFO_TYPE_VISIBLE,
		RenderingServer.VIEWPORT_RENDER_INFO_DRAW_CALLS_IN_FRAME,
	)


func _print_results() -> void:
	var frames := float(SAMPLE_FRAMES)
	var elapsed_ms := float(Time.get_ticks_usec() - sample_start_usec) / 1000.0
	print("STAGE_PROFILE_CPU frames=%d frame_setup_ms=%.6f viewport_cpu_ms=%.6f root_viewport_cpu_ms=%.6f rendering_cpu_total_ms=%.6f" % [
		SAMPLE_FRAMES,
		frame_setup_cpu_ms / frames,
		viewport_cpu_ms / frames,
		root_viewport_cpu_ms / frames,
		(frame_setup_cpu_ms + viewport_cpu_ms + root_viewport_cpu_ms) / frames,
	])
	if viewport_gpu_valid_samples > 0:
		var gpu_frames := float(viewport_gpu_valid_samples)
		print("STAGE_PROFILE_GPU valid=%d invalid=%d viewport_gpu_ms=%.6f root_viewport_gpu_ms=%.6f gpu_total_ms=%.6f" % [
			viewport_gpu_valid_samples,
			viewport_gpu_invalid_samples,
			viewport_gpu_ms / gpu_frames,
			root_viewport_gpu_ms / gpu_frames,
			(viewport_gpu_ms + root_viewport_gpu_ms) / gpu_frames,
		])
	else:
		print("STAGE_PROFILE_GPU valid=0 invalid=%d result=unavailable" % viewport_gpu_invalid_samples)
	print("STAGE_PROFILE_DRAW_CALLS average=%.3f" % (float(draw_calls) / frames))
	print("STAGE_PROFILE_FRAME frames=%d elapsed_ms=%.6f ms_per_frame=%.6f fps=%.3f" % [
		SAMPLE_FRAMES,
		elapsed_ms,
		elapsed_ms / frames,
		frames * 1000.0 / elapsed_ms,
	])


func _set_parameter_if_present(parameter: StringName, value: float) -> void:
	if parameter in model.get_parameters():
		model.set("parameters/%s" % parameter, value)
