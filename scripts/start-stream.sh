#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
runtime_dir="$project_dir/runtime"
mkdir -p "$runtime_dir"

if [[ -f "$runtime_dir/mediamtx.pid" ]] && kill -0 "$(<"$runtime_dir/mediamtx.pid")" 2>/dev/null; then
  echo "MediaMTX is already running."
else
  nohup "$project_dir/scripts/run-mediamtx.sh" >"$runtime_dir/mediamtx.log" 2>&1 &
  printf '%s\n' "$!" >"$runtime_dir/mediamtx.pid"
fi

for _ in {1..50}; do
  if ss -ltnH 'sport = :8554' | grep -q .; then
    break
  fi
  sleep 0.1
done

if ! ss -ltnH 'sport = :8554' | grep -q .; then
  echo "MediaMTX did not start; inspect runtime/mediamtx.log." >&2
  exit 1
fi

if [[ -f "$runtime_dir/camera.pid" ]] && kill -0 "$(<"$runtime_dir/camera.pid")" 2>/dev/null; then
  echo "Camera publisher is already running."
else
  nohup "$project_dir/scripts/publish-camera.sh" >"$runtime_dir/camera.log" 2>&1 &
  printf '%s\n' "$!" >"$runtime_dir/camera.pid"
fi

sleep 1
if ! kill -0 "$(<"$runtime_dir/camera.pid")" 2>/dev/null; then
  echo "Camera publisher did not start; inspect runtime/camera.log." >&2
  exit 1
fi

echo "Local RTSP stream: rtsp://127.0.0.1:8554/camera"
echo "From another device, replace 127.0.0.1 with this RK3588 host's IP address."
