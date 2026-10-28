#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
runtime_dir="$project_dir/runtime"
mediamtx_pid=""
publisher_pid=""

cleanup() {
  if [[ -n "$publisher_pid" ]] && kill -0 "$publisher_pid" 2>/dev/null; then
    kill -- "-$publisher_pid" 2>/dev/null || kill "$publisher_pid" 2>/dev/null || true
    wait "$publisher_pid" 2>/dev/null || true
  fi
  if [[ -n "$mediamtx_pid" ]] && kill -0 "$mediamtx_pid" 2>/dev/null; then
    kill "$mediamtx_pid" 2>/dev/null || true
    wait "$mediamtx_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$runtime_dir"

if ss -ltnH 'sport = :8554' | grep -q .; then
  if ! ss -ltnH 'sport = :8889' | grep -q .; then
    echo "An existing MediaMTX process has RTSP enabled without WebRTC." >&2
    echo "Stop it before starting the face3d browser stream." >&2
    exit 1
  fi
else
  "$project_dir/scripts/run-mediamtx.sh" >"$runtime_dir/mediamtx-face3d.log" 2>&1 &
  mediamtx_pid="$!"
fi

for _ in {1..50}; do
  if ss -ltnH 'sport = :8554' | grep -q . && ss -ltnH 'sport = :8889' | grep -q .; then
    break
  fi
  sleep 0.1
done

if ! ss -ltnH 'sport = :8554' | grep -q . || ! ss -ltnH 'sport = :8889' | grep -q .; then
  echo "MediaMTX did not expose RTSP and WebRTC; inspect runtime/mediamtx-face3d.log." >&2
  exit 1
fi

if ! command -v setsid >/dev/null 2>&1; then
  echo "setsid is required to supervise the face3d pipeline." >&2
  exit 1
fi

setsid bash "$project_dir/scripts/publish-face3d-browser.sh" \
  >"$runtime_dir/face3d-publisher.log" 2>&1 &
publisher_pid="$!"

stream_url="${VTB_FACE3D_RTSP_URL:-rtsp://127.0.0.1:8554/face3d}"
stream_ready=""
for _ in {1..30}; do
  if ! kill -0 "$publisher_pid" 2>/dev/null; then
    echo "The face3d publisher exited; inspect runtime/face3d-publisher.log." >&2
    exit 1
  fi
  if timeout 3 ffprobe -v error -rtsp_transport tcp \
    -select_streams v:0 -show_entries stream=codec_name \
    -of default=noprint_wrappers=1:nokey=1 "$stream_url" 2>/dev/null |
    grep -qx h264; then
    stream_ready=1
    break
  fi
  sleep 0.2
done

if [[ -z "$stream_ready" ]]; then
  echo "The face3d RTSP stream did not become readable; inspect runtime logs." >&2
  exit 1
fi

diagnostics_port="${VTB_FACE3D_DIAGNOSTICS_PORT:-8890}"
diagnostics_ready=""
for _ in {1..30}; do
  if timeout 2 curl -fsS "http://127.0.0.1:${diagnostics_port}/health" 2>/dev/null |
    grep -q '"status":"ok"'; then
    diagnostics_ready=1
    break
  fi
  sleep 0.1
done

if [[ -z "$diagnostics_ready" ]]; then
  echo "The latency diagnostics page did not become ready." >&2
  exit 1
fi

host_name="$(hostname -s)"
echo "Browser URL: http://${host_name}.local:8889/face3d"
echo "Latency URL: http://${host_name}.local:${diagnostics_port}/"
echo "WebRTC media port: UDP 8189"
echo "Press Ctrl-C to stop the publisher and the MediaMTX process started here."

wait "$publisher_pid"
