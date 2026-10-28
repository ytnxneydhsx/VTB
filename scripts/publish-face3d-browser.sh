#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
python_bin="${VTB_FACE3D_PYTHON:-$project_dir/build/openseeface-venv/bin/python}"
capture_device="${VTB_FACE3D_CAPTURE:-/dev/video0}"
capture_width="${VTB_FACE3D_CAPTURE_WIDTH:-1280}"
capture_height="${VTB_FACE3D_CAPTURE_HEIGHT:-720}"
capture_fps="${VTB_FACE3D_CAPTURE_FPS:-60}"
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

"$python_bin" "$project_dir/tools/openseeface_3d_preview.py" \
  --capture "$capture_device" \
  --width "$capture_width" \
  --height "$capture_height" \
  --fps "$capture_fps" \
  --canvas-width "$canvas_width" \
  --canvas-height "$canvas_height" \
  --timed-raw-output \
  --backend "$inference_backend" \
  --rknn-model-dir "$rknn_model_dir" \
  --no-window | \
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
