#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
python_bin="${VTB_FACE3D_PYTHON:-$project_dir/build/openseeface-venv/bin/python}"
capture_device="${VTB_FACE3D_CAPTURE:-/dev/video0}"
capture_width="${VTB_FACE3D_CAPTURE_WIDTH:-1280}"
capture_height="${VTB_FACE3D_CAPTURE_HEIGHT:-720}"
capture_fps="${VTB_FACE3D_CAPTURE_FPS:-60}"
capture_buffers="${VTB_FACE3D_CAPTURE_BUFFERS:-4}"
canvas_width="${VTB_FACE3D_CANVAS_WIDTH:-1280}"
canvas_height="${VTB_FACE3D_CANVAS_HEIGHT:-720}"
stream_fps="${VTB_FACE3D_STREAM_FPS:-30}"
stream_url="${VTB_FACE3D_RTSP_URL:-rtsp://127.0.0.1:8554/face3d}"
inference_backend="${VTB_FACE3D_BACKEND:-rknn}"
rknn_model_dir="${VTB_FACE3D_RKNN_MODEL_DIR:-$project_dir/models/openseeface}"
publisher_bin="${VTB_MPP_OUTPUT_PUBLISHER:-$project_dir/build/vtb-mpp-output-publisher}"
stream_bitrate="${VTB_FACE3D_STREAM_BITRATE:-1200000}"
dma_heap="${VTB_FACE3D_DMA_HEAP:-/dev/dma_heap/system-uncached}"
diagnostics_port="${VTB_FACE3D_DIAGNOSTICS_PORT:-8890}"
diagnostics_page="${VTB_FACE3D_DIAGNOSTICS_PAGE:-$project_dir/web/face3d-latency.html}"
live2d_udp="${VTB_FACE3D_LIVE2D_UDP:-}"
live2d_angle_x_sign="${VTB_FACE3D_LIVE2D_ANGLE_X_SIGN:--1}"
live2d_angle_y_sign="${VTB_FACE3D_LIVE2D_ANGLE_Y_SIGN:--1}"
live2d_angle_z_sign="${VTB_FACE3D_LIVE2D_ANGLE_Z_SIGN:--1}"
live2d_control_address="${VTB_FACE3D_LIVE2D_CONTROL_ADDRESS:-192.168.77.2}"
live2d_control_port="${VTB_FACE3D_LIVE2D_CONTROL_PORT:-11575}"
live2d_control_origin="${VTB_FACE3D_LIVE2D_CONTROL_ORIGIN:-http://192.168.77.1:8900}"
performance_stats="${VTB_FACE3D_PERFORMANCE_STATS:-0}"
performance_report_frames="${VTB_FACE3D_PERFORMANCE_REPORT_FRAMES:-300}"

if [[ "$performance_stats" == "1" &&
      ! "$performance_report_frames" =~ ^[1-9][0-9]*$ ]]; then
  echo "VTB_FACE3D_PERFORMANCE_REPORT_FRAMES must be a positive integer." >&2
  exit 1
fi

if [[ ! -x "$python_bin" ]]; then
  echo "Python environment is missing: $python_bin" >&2
  echo "Create it with the commands documented in README.md." >&2
  exit 1
fi

if [[ ! -e "$capture_device" ]]; then
  echo "Capture device is missing: $capture_device" >&2
  exit 1
fi

if [[ ! -x "$publisher_bin" ]]; then
  echo "MPP output publisher is missing: $publisher_bin" >&2
  exit 1
fi

if [[ ! -f "$diagnostics_page" ]]; then
  echo "Latency diagnostics page is missing: $diagnostics_page" >&2
  exit 1
fi

publisher_command=("$publisher_bin")
if [[ ! -r /dev/mpp_service || ! -w /dev/mpp_service ||
      ! -r "$dma_heap" || ! -w "$dma_heap" ]]; then
  if ! sudo -n true >/dev/null 2>&1; then
    echo "MPP and DMA heap access require non-interactive sudo on this BSP." >&2
    exit 1
  fi
  publisher_command=(sudo -n "$publisher_bin")
fi

tracker_command=(
  "$python_bin"
  "$project_dir/support/tools/openseeface_3d_preview.py"
  --capture "$capture_device"
  --width "$capture_width"
  --height "$capture_height"
  --fps "$capture_fps"
  --capture-buffers "$capture_buffers"
  --canvas-width "$canvas_width"
  --canvas-height "$canvas_height"
  --timed-raw-output
  --backend "$inference_backend"
  --rknn-model-dir "$rknn_model_dir"
  --no-window
)

if [[ -n "$live2d_udp" ]]; then
  tracker_command+=(
    --live2d-udp "$live2d_udp"
    --live2d-angle-x-sign "$live2d_angle_x_sign"
    --live2d-angle-y-sign "$live2d_angle_y_sign"
    --live2d-angle-z-sign "$live2d_angle_z_sign"
    --live2d-control-address "$live2d_control_address"
    --live2d-control-port "$live2d_control_port"
    --live2d-control-origin "$live2d_control_origin"
  )
fi

if [[ "$performance_stats" == "1" ]]; then
  tracker_command+=(
    --performance-stats
    --performance-report-interval-frames "$performance_report_frames"
  )
fi

"${tracker_command[@]}" | \
exec "${publisher_command[@]}" \
  --rtsp-url "$stream_url" \
  --stream-id face3d-video \
  --width "$canvas_width" \
  --height "$canvas_height" \
  --fps "$stream_fps" \
  --bitrate "$stream_bitrate" \
  --gop "$stream_fps" \
  --queue 8 \
  --dma-heap "$dma_heap" \
  --timed-input \
  --diagnostics-port "$diagnostics_port" \
  --diagnostics-page "$diagnostics_page"
