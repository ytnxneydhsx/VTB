#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
	echo "usage: $0 /absolute/path/to/godot" >&2
	exit 2
fi

godot_source=$1
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

if [[ ! -f "$godot_source/platform/linuxbsd/os_linuxbsd.cpp" ]]; then
	echo "Godot LinuxBSD source is missing under: $godot_source" >&2
	exit 3
fi

patches=(
	0001-linuxbsd-add-headless-egl-pbuffer-display-server.patch
	0002-egl-select-rk3588-pbuffer-config.patch
	0003-headless-gbm-skip-client-extension-query.patch
	0004-egl-guard-unavailable-blob-cache.patch
	0005-egl-reuse-initialized-display.patch
	0006-headless-egl-use-gbm-display-for-window.patch
)
patch_paths=()
for patch_name in "${patches[@]}"; do
	patch_paths+=("$script_dir/$patch_name")
done
series_hash=$(sha256sum "${patch_paths[@]}" | sha256sum | awk '{print $1}')
marker_path="$godot_source/.openvt-headless-egl-0001-0006.sha256"

if [[ -f "$marker_path" ]] && [[ "$(<"$marker_path")" == "$series_hash" ]]; then
	echo "Headless EGL patch series is already present in: $godot_source"
	exit 0
fi

display_source="$godot_source/platform/linuxbsd/headless_egl/display_server_headless_egl.cpp"
egl_source="$godot_source/drivers/egl/egl_manager.cpp"
egl_header="$godot_source/drivers/egl/egl_manager.h"
if [[ -f "$display_source" ]] \
		&& grep -q "return nullptr;" "$display_source" \
		&& grep -q "attribs_pbuffer" "$egl_source" \
		&& grep -q "blob_cache_functions_available" "$egl_source" \
		&& grep -q "preinitialized_egl_display" "$egl_header" \
		&& grep -q "window_create(DisplayServerEnums::MAIN_WINDOW_ID, gbm" "$display_source"; then
	printf '%s\n' "$series_hash" > "$marker_path"
	echo "Headless EGL patch series was already present; marker recorded in: $godot_source"
	exit 0
fi

for patch_name in "${patches[@]}"; do
	patch_path="$script_dir/$patch_name"
	if patch --dry-run --forward --batch -d "$godot_source" -p1 < "$patch_path" >/dev/null 2>&1; then
		patch --forward --batch -d "$godot_source" -p1 < "$patch_path"
		continue
	fi
	if patch --dry-run --reverse --batch -d "$godot_source" -p1 < "$patch_path" >/dev/null 2>&1; then
		echo "already applied: $patch_name"
		continue
	fi
	echo "patch cannot be applied cleanly: $patch_name" >&2
	exit 4
done

printf '%s\n' "$series_hash" > "$marker_path"
echo "Headless EGL patch series is present in: $godot_source"
