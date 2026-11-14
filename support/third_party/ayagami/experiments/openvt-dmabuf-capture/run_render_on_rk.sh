#!/bin/bash
set -eu

task_output_dir=${OPENVT_DMABUF_OUTPUT_DIR:-/home/huangnv/openvt-dmabuf-render-20261027-01}
godot_bin=${OPENVT_GODOT_BIN:-/home/huangnv/tools/godot-ayagami-dmabuf-4.7.1/godot-ayagami-dmabuf-4.7.1-linux.arm64}
project_dir=${OPENVT_GODOT_PROJECT_DIR:-/home/huangnv/open-vt-test/support/third_party/ayagami}
scene_path=res://experiments/openvt-dmabuf-capture/render_dmabuf_video.tscn
h264_path="$task_output_dir/openvt-ayagami-dmabuf-1920x1080-30fps.h264"
mp4_path="$task_output_dir/openvt-ayagami-dmabuf-1920x1080-30fps.mp4"
run_log="$task_output_dir/godot-render.log"
runtime_log="$task_output_dir/runtime-metrics.txt"
xauthority_path=/home/huangnv/.Xauthority
mali_root=/opt/rockchip/libmali-g13p0-x11-gbm-1.9-1/usr/lib/aarch64-linux-gnu
profile_enabled=${OPENVT_PROFILE:-0}
gpu_profile_enabled=${OPENVT_GODOT_GPU_PROFILE:-0}
vsync_disabled=${OPENVT_DISABLE_VSYNC:-0}
profile_args=()
if [ "$gpu_profile_enabled" = "1" ]; then
	profile_args+=(--gpu-profile)
fi
if [ "$vsync_disabled" = "1" ]; then
	profile_args+=(--disable-vsync)
fi

if [ -e "$task_output_dir" ]; then
	echo "output directory already exists: $task_output_dir" >&2
	exit 20
fi

for required_path in \
	"$godot_bin" \
	"$project_dir/project.godot" \
	"$project_dir/experiments/openvt-dmabuf-capture/render_dmabuf_video.gd" \
	"$project_dir/experiments/openvt-dmabuf-capture/render_dmabuf_video.tscn" \
	"$xauthority_path" \
	"$mali_root/mali/libEGL.so.1" \
	/dev/dma_heap/system-uncached \
	/dev/rga \
	/dev/mpp_service; do
	if [ ! -e "$required_path" ]; then
		echo "required path is missing: $required_path" >&2
		exit 21
	fi
done

mkdir -p "$task_output_dir"

{
	echo "date=$(date --iso-8601=seconds)"
	echo "hostname=$(hostname)"
	echo "architecture=$(uname -m)"
	echo "kernel=$(uname -r)"
	echo "godot_bin=$godot_bin"
	echo "project_dir=$project_dir"
	echo "scene_path=$scene_path"
	echo "h264_path=$h264_path"
	echo "mp4_path=$mp4_path"
	echo "display=:0"
	echo "mali_default_winsys=x11"
	echo "profile_enabled=$profile_enabled"
	echo "godot_gpu_profile_enabled=$gpu_profile_enabled"
	echo "vsync_disabled=$vsync_disabled"
} > "$task_output_dir/run-manifest.txt"

sha256sum \
	"$godot_bin" \
	"$project_dir/experiments/openvt-dmabuf-capture/render_dmabuf_video.gd" \
	"$project_dir/experiments/openvt-dmabuf-capture/render_dmabuf_video.tscn" \
	> "$task_output_dir/input-sha256.txt"

wall_start_ns=$(date +%s%N)
sudo -n env \
	OPENVT_DMABUF_OUTPUT_PATH="$h264_path" \
	OPENVT_PROFILE="$profile_enabled" \
	DISPLAY=:0 \
	XAUTHORITY="$xauthority_path" \
	MALI_DEFAULT_WINSYS=x11 \
	LD_LIBRARY_PATH="$mali_root/mali:$mali_root:/usr/lib/aarch64-linux-gnu" \
	"$godot_bin" \
		--path "$project_dir" \
		--display-driver x11 \
		--rendering-driver opengl3_es \
		--single-window \
		--resolution 1x1 \
		--position 1200,0 \
		--fixed-fps 30 \
		"${profile_args[@]}" \
		"$scene_path" \
	> "$run_log" 2>&1 &
godot_pid=$!

(
	while kill -0 "$godot_pid" 2>/dev/null; do
		metric_timestamp=$(date +%s.%N)
		metric_rss_kib=unavailable
		metric_process_ticks=unavailable
		metric_gpu=unavailable
		if [ -r "/proc/$godot_pid/status" ]; then
			metric_rss_kib=$(awk '/^VmRSS:/ {print $2}' "/proc/$godot_pid/status")
		fi
		if [ -r "/proc/$godot_pid/stat" ]; then
			metric_process_ticks=$(awk '{print $14 + $15}' "/proc/$godot_pid/stat")
		fi
		if [ -r /sys/class/devfreq/fb000000.gpu/load ]; then
			metric_gpu=$(cat /sys/class/devfreq/fb000000.gpu/load)
		fi
		printf '%s rss_kib=%s process_ticks=%s gpu=%s\n' \
			"$metric_timestamp" "$metric_rss_kib" "$metric_process_ticks" "$metric_gpu"
		sleep 0.2
	done
) > "$runtime_log" 2>&1 &
runtime_sampler_pid=$!

set +e
wait "$godot_pid"
godot_exit=$?
capture_end_ns=$(date +%s%N)
wait "$runtime_sampler_pid" 2>/dev/null
set -e

printf '%d\n' "$godot_exit" > "$task_output_dir/godot.exit"
if [ "$godot_exit" -ne 0 ]; then
	echo "Godot DMA-BUF render failed with exit code $godot_exit" >&2
	exit "$godot_exit"
fi

if [ ! -s "$h264_path" ]; then
	echo "H.264 output is missing or empty: $h264_path" >&2
	exit 22
fi

ffmpeg -hide_banner -loglevel error -y -framerate 30 -i "$h264_path" -c copy "$mp4_path"
ffprobe \
	-v error \
	-count_frames \
	-select_streams v:0 \
	-show_entries stream=codec_name,width,height,pix_fmt,r_frame_rate,avg_frame_rate,nb_read_frames:format=duration,size \
	-of default=noprint_wrappers=1 \
	"$mp4_path" \
	> "$task_output_dir/ffprobe.txt"

frame_count=$(awk -F= '$1 == "nb_read_frames" {print $2}' "$task_output_dir/ffprobe.txt")
if [ "$frame_count" -ne 300 ]; then
	echo "unexpected encoded frame count: $frame_count" >&2
	exit 23
fi

ffmpeg -hide_banner -loglevel error -y -i "$mp4_path" -f null - \
	> "$task_output_dir/decode.log" 2>&1

for timestamp in 1.0 2.5 4.0 6.0 8.0 9.9; do
	label=$(printf '%04d' "$(awk -v time="$timestamp" 'BEGIN {printf "%d", time * 30}')")
	ffmpeg -hide_banner -loglevel error -y -ss "$timestamp" -i "$mp4_path" -frames:v 1 \
		"$task_output_dir/preview-$label.png"
done

{
	echo "wall_start_ns=$wall_start_ns"
	echo "capture_end_ns=$capture_end_ns"
	awk -v start="$wall_start_ns" -v end="$capture_end_ns" 'BEGIN {printf "capture_wall_seconds=%.6f\n", (end - start) / 1000000000}'
} > "$task_output_dir/time.txt"

sha256sum \
	"$h264_path" \
	"$mp4_path" \
	"$run_log" \
	"$runtime_log" \
	"$task_output_dir/ffprobe.txt" \
	"$task_output_dir/decode.log" \
	"$task_output_dir"/preview-*.png \
	> "$task_output_dir/SHA256SUMS"

echo "Godot DMA-BUF render complete: $mp4_path"

