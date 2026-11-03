#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
project_root=$(cd "$script_dir/.." && pwd)
generated_html="$project_root/build/docs/video-input-doxygen/html"
expected_html="$project_root/build/docs/video-input-doxygen/html"

if [[ "$generated_html" != "$expected_html" ]]; then
	printf 'Refusing to clean unexpected documentation path: %s\n' "$generated_html" >&2
	exit 1
fi

if [[ -d "$generated_html" ]]; then
	find "$generated_html" -mindepth 1 -maxdepth 1 -exec rm -rf -- {} +
fi

cd "$project_root"
doxygen docs/Doxyfile.video-input
