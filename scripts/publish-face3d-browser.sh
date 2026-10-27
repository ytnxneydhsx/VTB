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

if [[ ! -x "$python_bin" ]]; then
  echo "Python environment is missing: $python_bin" >&2
  echo "Create it with the commands documented in README.md." >&2
  exit 1
fi

if [[ ! -e "$capture_device" ]]; then
  echo "Capture device is missing: $capture_device" >&2
  exit 1
fi

if ! command -v ffmpeg >/dev/null 2>&1; then
  echo "ffmpeg is required." >&2
  exit 1
fi

exec ffmpeg \
  -hide_banner \
  -loglevel warning \
  -fflags nobuffer \
  -f rawvideo \
  -pixel_format bgr24 \
  -video_size "${canvas_width}x${canvas_height}" \
  -framerate "$stream_fps" \
  -i <(
    exec "$python_bin" "$project_dir/tools/openseeface_3d_preview.py" \
      --capture "$capture_device" \
      --width "$capture_width" \
      --height "$capture_height" \
      --fps "$capture_fps" \
      --canvas-width "$canvas_width" \
      --canvas-height "$canvas_height" \
      --raw-output \
      --no-window
  ) \
  -an \
  -vf format=yuv420p \
  -fps_mode passthrough \
  -c:v libx264 \
  -preset ultrafast \
  -tune zerolatency \
  -profile:v baseline \
  -level:v 3.1 \
  -g "$stream_fps" \
  -keyint_min "$stream_fps" \
  -sc_threshold 0 \
  -bf 0 \
  -b:v 1200k \
  -maxrate 1200k \
  -bufsize 600k \
  -flush_packets 1 \
  -f rtsp \
  -rtsp_transport tcp \
  "$stream_url"
