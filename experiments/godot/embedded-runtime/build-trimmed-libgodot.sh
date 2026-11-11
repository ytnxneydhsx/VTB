#!/usr/bin/env bash

set -eu

if [ "$#" -ne 1 ]; then
	echo "usage: $0 /absolute/path/to/godot-source" >&2
	exit 2
fi

: "${OPENVT_BOARD_SYSROOT:?Set OPENVT_BOARD_SYSROOT to the ARM64 Rockchip sysroot}"
: "${OPENVT_BUILD_JOBS:?Set OPENVT_BUILD_JOBS after checking the build server load and memory}"
: "${CC:?Set CC to the pinned ARM64 C compiler}"
: "${CXX:?Set CXX to the pinned ARM64 C++ compiler}"

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
vtb_source_root=$(CDPATH= cd -- "$script_dir/../../.." && pwd)
godot_source=$1
scons_bin=${SCONS:-scons}

if [ ! -f "$godot_source/SConstruct" ]; then
	echo "Godot SConstruct not found under: $godot_source" >&2
	exit 2
fi

export OPENVT_VTB_SOURCE_ROOT="$vtb_source_root"

cd "$godot_source"
exec "$scons_bin" "-j$OPENVT_BUILD_JOBS" \
	platform=linuxbsd \
	arch=arm64 \
	target=template_release \
	library_type=shared_library \
	production=yes \
	optimize=size \
	lto=none \
	disable_path_overrides=no \
	debug_symbols=no \
	progress=no \
	warnings=no \
	x11=no \
	wayland=no \
	vulkan=no \
	opengl3=yes \
	use_static_cpp=no \
	modules_enabled_by_default=no \
	module_openvt_dmabuf_capture_enabled=yes \
	disable_3d=yes \
	disable_advanced_gui=yes \
	disable_physics_2d=yes \
	disable_physics_3d=yes \
	disable_navigation_2d=yes \
	disable_navigation_3d=yes \
	disable_xr=yes \
	minizip=no \
	brotli=no \
	accesskit=no \
	sdl=no \
	angle=no \
	alsa=no \
	pulseaudio=no \
	dbus=no \
	speechd=no \
	fontconfig=no \
	udev=no \
	touch=no \
	CC="$CC" \
	CXX="$CXX" \
	LINK="$CXX"
