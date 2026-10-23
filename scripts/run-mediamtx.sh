#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ ! -x "$project_dir/bin/mediamtx" ]]; then
  echo "MediaMTX is missing. Run ./scripts/install-mediamtx.sh first." >&2
  exit 1
fi

exec "$project_dir/bin/mediamtx" "$project_dir/mediamtx.yml"
