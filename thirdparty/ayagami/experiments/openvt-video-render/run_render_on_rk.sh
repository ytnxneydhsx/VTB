#!/bin/bash
set -u

task_output_dir=${OPENVT_RENDER_OUTPUT_DIR:-/home/huangnv/openvt-video-render-20261027-01}
godot_bin=/home/huangnv/tools/godot-ayagami-4.7.1/godot-ayagami-4.7.1-linux.arm64
project_dir=/home/huangnv/open-vt-test/thirdparty/ayagami
scene_path=res://experiments/openvt-video-render/render_video.tscn
video_path="$task_output_dir/openvt-ayagami-1920x1080-30fps-10s.avi"
run_log="$task_output_dir/godot-render.log"
ffmpeg_log="$task_output_dir/ffmpeg.log"
time_log="$task_output_dir/time.txt"
runtime_log="$task_output_dir/runtime-metrics.txt"
xauthority_path=/home/huangnv/.Xauthority
mali_root=/opt/rockchip/libmali-g13p0-x11-gbm-1.9-1/usr/lib/aarch64-linux-gnu

if [ -e "$task_output_dir" ]; then
	echo "output directory already exists: $task_output_dir" >&2
	exit 20
fi

for required_path in \
	"$godot_bin" \
	"$project_dir/project.godot" \
	"$project_dir/experiments/openvt-video-render/render_video.gd" \
	"$project_dir/experiments/openvt-video-render/render_video.tscn" \
	"$xauthority_path" \
	"$mali_root/mali/libEGL.so.1"; do
	if [ ! -e "$required_path" ]; then
		echo "required path is missing: $required_path" >&2
		exit 21
	fi
done

mkdir -p "$task_output_dir/frames"

{
	echo "date=$(date --iso-8601=seconds)"
	echo "hostname=$(hostname)"
	echo "architecture=$(uname -m)"
	echo "kernel=$(uname -r)"
	echo "godot_bin=$godot_bin"
	echo "project_dir=$project_dir"
	echo "scene_path=$scene_path"
	echo "video_path=$video_path"
	echo "display=:0"
	echo "xauthority=$xauthority_path"
	echo "mali_default_winsys=x11"
} > "$task_output_dir/run-manifest.txt"

sha256sum \
	"$godot_bin" \
	"$project_dir/experiments/openvt-video-render/render_video.gd" \
	"$project_dir/experiments/openvt-video-render/render_video.tscn" \
	> "$task_output_dir/input-sha256.txt"

wall_start_ns=$(date +%s%N)
OPENVT_RENDER_OUTPUT_DIR="$task_output_dir" \
DISPLAY=:0 \
XAUTHORITY="$xauthority_path" \
MALI_DEFAULT_WINSYS=x11 \
LD_LIBRARY_PATH="$mali_root/mali:$mali_root" \
"$godot_bin" \
	--path "$project_dir" \
	--display-driver x11 \
	--rendering-driver opengl3_es \
	--single-window \
	--resolution 1x1 \
	--position 1200,0 \
	--fixed-fps 30 \
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
			"$metric_timestamp" \
			"$metric_rss_kib" \
			"$metric_process_ticks" \
			"$metric_gpu"
		sleep 0.2
	done
) > "$runtime_log" 2>&1 &
runtime_sampler_pid=$!

wait "$godot_pid"
godot_exit=$?
capture_end_ns=$(date +%s%N)

wait "$runtime_sampler_pid" 2>/dev/null

printf '%d\n' "$godot_exit" > "$task_output_dir/godot.exit"

if [ "$godot_exit" -ne 0 ]; then
	echo "Godot render failed with exit code $godot_exit" >&2
	exit "$godot_exit"
fi

frame_count=$(find "$task_output_dir/frames" -maxdepth 1 -type f -name 'frame-*.jpg' | wc -l | tr -d ' ')
if [ "$frame_count" -ne 300 ]; then
	echo "unexpected JPEG frame count: $frame_count" >&2
	exit 22
fi

mux_start_ns=$(date +%s%N)
ffmpeg \
	-hide_banner \
	-loglevel info \
	-y \
	-framerate 30 \
	-start_number 1 \
	-i "$task_output_dir/frames/frame-%04d.jpg" \
	-frames:v 300 \
	-c:v copy \
	-an \
	-f avi \
	"$video_path" \
	> "$ffmpeg_log" 2>&1
ffmpeg_exit=$?
mux_end_ns=$(date +%s%N)

printf '%d\n' "$ffmpeg_exit" > "$task_output_dir/ffmpeg.exit"
if [ "$ffmpeg_exit" -ne 0 ]; then
	echo "FFmpeg mux failed with exit code $ffmpeg_exit" >&2
	exit "$ffmpeg_exit"
fi

if [ ! -s "$video_path" ]; then
	echo "video output is missing or empty: $video_path" >&2
	exit 23
fi

{
	echo "wall_start_ns=$wall_start_ns"
	echo "capture_end_ns=$capture_end_ns"
	echo "mux_start_ns=$mux_start_ns"
	echo "mux_end_ns=$mux_end_ns"
	awk -v start="$wall_start_ns" -v end="$capture_end_ns" 'BEGIN {printf "capture_wall_seconds=%.6f\n", (end - start) / 1000000000}'
	awk -v start="$mux_start_ns" -v end="$mux_end_ns" 'BEGIN {printf "mux_wall_seconds=%.6f\n", (end - start) / 1000000000}'
	awk -v start="$wall_start_ns" -v end="$mux_end_ns" 'BEGIN {printf "total_wall_seconds=%.6f\n", (end - start) / 1000000000}'
} > "$time_log"

ffprobe \
	-v error \
	-select_streams v:0 \
	-show_entries stream=codec_name,width,height,r_frame_rate,avg_frame_rate,nb_frames:format=duration,size \
	-of default=noprint_wrappers=1 \
	"$video_path" \
	> "$task_output_dir/ffprobe.txt"

sha256sum \
	"$video_path" \
	"$run_log" \
	"$ffmpeg_log" \
	"$time_log" \
	"$runtime_log" \
	"$task_output_dir/ffprobe.txt" \
	> "$task_output_dir/SHA256SUMS"

echo "render complete: $video_path"
