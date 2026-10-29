#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
bind_address="${VTB_MONITOR_BIND_ADDRESS:-192.168.77.1}"
port="${VTB_MONITOR_PORT:-8900}"
page_name="dual-stream-monitor.html"

if ! command -v python3 >/dev/null 2>&1; then
  echo "python3 is required to serve the dual-stream monitor." >&2
  exit 1
fi

echo "Dual-stream monitor: http://${bind_address}:${port}/${page_name}"
exec python3 -m http.server "$port" \
  --bind "$bind_address" \
  --directory "$project_dir/web"
