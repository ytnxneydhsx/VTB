#!/bin/bash
set -eu

task_output_dir=${OPENVT_DMABUF_OUTPUT_DIR:-/home/huangnv/openvt-dmabuf-render-20261027-01}
godot_bin=${OPENVT_GODOT_BIN:-/home/huangnv/tools/godot-ayagami-live2d-rtsp-4.7.1/godot-ayagami-live2d-rtsp-4.7.1-linux.arm64}
project_dir=${OPENVT_GODOT_PROJECT_DIR:-/home/huangnv/open-vt-test/thirdparty/ayagami}
scene_path=res://experiments/openvt-dmabuf-capture/render_dmabuf_video.tscn
run_log="$task_output_dir/godot-render.log"
runtime_log="$task_output_dir/runtime-metrics.txt"
xauthority_path=/home/huangnv/.Xauthority
mali_root=/opt/rockchip/libmali-g13p0-x11-gbm-1.9-1/usr/lib/aarch64-linux-gnu
profile_enabled=${OPENVT_PROFILE:-0}
profile_report_frames=${OPENVT_PROFILE_REPORT_FRAMES:-300}
ayagami_profile_enabled=0
if [ "$profile_enabled" = "1" ]; then
	ayagami_profile_enabled=1
fi
gpu_profile_enabled=${OPENVT_GODOT_GPU_PROFILE:-0}
vsync_disabled=${OPENVT_DISABLE_VSYNC:-0}
live2d_total_frames=${OPENVT_LIVE2D_TOTAL_FRAMES:-300}
rtsp_url=${OPENVT_RTSP_URL:-rtsp://127.0.0.1:8554/live2d}
profile_args=()
if [ "$profile_enabled" = "1" ]; then
	case "$profile_report_frames" in
		''|*[!0-9]*|0)
			echo "OPENVT_PROFILE_REPORT_FRAMES must be a positive integer" >&2
			exit 20
			;;
	esac
fi
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
	echo "display=:0"
	echo "mali_default_winsys=x11"
	echo "profile_enabled=$profile_enabled"
	echo "profile_report_frames=$profile_report_frames"
	echo "ayagami_profile_enabled=$ayagami_profile_enabled"
	echo "godot_gpu_profile_enabled=$gpu_profile_enabled"
	echo "vsync_disabled=$vsync_disabled"
	echo "live2d_total_frames=$live2d_total_frames"
	echo "rtsp_output_configured=true"
} > "$task_output_dir/run-manifest.txt"

sha256sum \
	"$godot_bin" \
	"$project_dir/experiments/openvt-dmabuf-capture/render_dmabuf_video.gd" \
	"$project_dir/experiments/openvt-dmabuf-capture/render_dmabuf_video.tscn" \
	> "$task_output_dir/input-sha256.txt"

wall_start_ns=$(date +%s%N)
sudo -n env -u OPENVT_FACE_SOURCE \
	OPENVT_RTSP_URL="$rtsp_url" \
	OPENVT_PROFILE="$profile_enabled" \
	OPENVT_PROFILE_REPORT_FRAMES="$profile_report_frames" \
	OPENVT_AYAGAMI_PROFILE="$ayagami_profile_enabled" \
	OPENVT_LIVE2D_TOTAL_FRAMES="$live2d_total_frames" \
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

if ! grep -Eq 'OPENVT_RTSP_RESULT .*written=[1-9][0-9]*' "$run_log"; then
	echo "RTSP output did not report any written packets" >&2
	exit 22
fi

{
	echo "wall_start_ns=$wall_start_ns"
	echo "capture_end_ns=$capture_end_ns"
	awk -v start="$wall_start_ns" -v end="$capture_end_ns" 'BEGIN {printf "capture_wall_seconds=%.6f\n", (end - start) / 1000000000}'
} > "$task_output_dir/time.txt"

sha256sum \
	"$run_log" \
	"$runtime_log" \
	"$task_output_dir/time.txt" \
	> "$task_output_dir/SHA256SUMS"

echo "Godot DMA-BUF RTSP publish complete"

