#!/usr/bin/env bash
set -euo pipefail

exec ffmpeg \
  -f v4l2 \
  -input_format mjpeg \
  -video_size 1280x720 \
  -framerate 30 \
  -i /dev/video0 \
  -an \
  -c:v libx264 \
  -preset ultrafast \
  -tune zerolatency \
  -profile:v baseline \
  -level:v 3.1 \
  -pix_fmt yuv420p \
  -b:v 2500k \
  -maxrate 2500k \
  -bufsize 5000k \
  -g 30 \
  -keyint_min 30 \
  -sc_threshold 0 \
  -bf 0 \
  -f rtsp \
  -rtsp_transport tcp \
  rtsp://127.0.0.1:8554/camera
