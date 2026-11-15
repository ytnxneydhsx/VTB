#!/usr/bin/env bash

set -eu

: "${GSTREAMER_SOURCE_ROOT:?Set the directory containing the four 1.28.9 source trees}"
: "${GSTREAMER_BUILD_ROOT:?Set a task-owned build directory}"
: "${GSTREAMER_STAGING_ROOT:?Set a task-owned DESTDIR/sysroot overlay}"
: "${GSTREAMER_MESON_PYTHON:?Set the absolute path to the host Python interpreter}"
: "${GSTREAMER_MESON_PYTHONPATH:?Set the unpacked Meson 1.7.2 wheel directory}"
: "${GSTREAMER_HOST_TOOLS:?Set the task-local directory containing GLib code generators}"
: "${GSTREAMER_GUDEV_ROOT:?Set the merged ARM64 libgudev runtime/development package root}"
: "${VTB_GODOT_SDK:?Set the relocated Buildroot/Godot SDK root}"
: "${GSTREAMER_BUILD_JOBS:?Set parallelism after checking server resources}"

prefix=/opt/vtb/gstreamer-1.28.9
libdir=lib/aarch64-linux-gnu
toolchain_root="$VTB_GODOT_SDK"
target_sysroot="$VTB_GODOT_SDK/aarch64-godot-linux-gnu/sysroot"
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cross_file="$GSTREAMER_BUILD_ROOT/aarch64-godot-linux-gnu.ini"
gst_plugins_bad_source="$GSTREAMER_SOURCE_ROOT/gst-plugins-bad-1.28.9"
gst_plugins_good_source="$GSTREAMER_SOURCE_ROOT/gst-plugins-good-1.28.9"

mkdir -p "$GSTREAMER_BUILD_ROOT" "$GSTREAMER_STAGING_ROOT"

apply_patch_set() {
	patch_source=$1
	shift
	for gstreamer_patch in "$@"; do
		if git -C "$patch_source" apply --check "$gstreamer_patch" >/dev/null 2>&1; then
			git -C "$patch_source" apply "$gstreamer_patch"
		else
			printf 'GStreamer patch does not apply cleanly: %s\n' "$(basename "$gstreamer_patch")" >&2
			exit 1
		fi
	done
}

patch_stamp="$GSTREAMER_SOURCE_ROOT/.vtb-gstreamer-patches-1.28.9.sha256"
patch_set_hash=$(
	sha256sum "$script_dir"/patches/000*.patch "$script_dir"/patches/100*.patch |
		sha256sum | cut -d ' ' -f 1
)

if [ -f "$patch_stamp" ]; then
	applied_patch_set_hash=$(cat "$patch_stamp")
	if [ "$applied_patch_set_hash" != "$patch_set_hash" ]; then
		printf 'GStreamer source was patched with a different project patch set.\n' >&2
		exit 1
	fi
	printf 'GStreamer project patch set is already applied.\n'
else
	apply_patch_set "$gst_plugins_bad_source" "$script_dir"/patches/000*.patch
	apply_patch_set "$gst_plugins_good_source" "$script_dir"/patches/100*.patch
	printf '%s\n' "$patch_set_hash" > "$patch_stamp"
fi

if [ ! -e "$GSTREAMER_STAGING_ROOT/usr" ]; then
	ln -s "$target_sysroot/usr" "$GSTREAMER_STAGING_ROOT/usr"
fi

gudev_include_source="$GSTREAMER_GUDEV_ROOT/usr/include/gudev-1.0"
gudev_library_source="$GSTREAMER_GUDEV_ROOT/usr/lib/aarch64-linux-gnu"
gudev_prefix_root="$GSTREAMER_STAGING_ROOT$prefix"
test -f "$gudev_include_source/gudev/gudev.h"
test -f "$gudev_library_source/libgudev-1.0.so.0.3.0"
test -f "$gudev_library_source/pkgconfig/gudev-1.0.pc"
mkdir -p \
	"$gudev_prefix_root/include" \
	"$gudev_prefix_root/$libdir/pkgconfig"
cp -a "$gudev_include_source" "$gudev_prefix_root/include/"
cp -a "$gudev_library_source"/libgudev-1.0.so* "$gudev_prefix_root/$libdir/"
sed \
	-e "s|^prefix=/usr$|prefix=$prefix|" \
	"$gudev_library_source/pkgconfig/gudev-1.0.pc" \
	> "$gudev_prefix_root/$libdir/pkgconfig/gudev-1.0.pc"
if [ ! -e "$GSTREAMER_STAGING_ROOT/lib" ]; then
	ln -s "$target_sysroot/lib" "$GSTREAMER_STAGING_ROOT/lib"
fi
if [ -d "$target_sysroot/lib64" ] && [ ! -e "$GSTREAMER_STAGING_ROOT/lib64" ]; then
	ln -s "$target_sysroot/lib64" "$GSTREAMER_STAGING_ROOT/lib64"
fi

sed \
	-e "s|@TOOLCHAIN_ROOT@|$toolchain_root|g" \
	-e "s|@TARGET_SYSROOT@|$target_sysroot|g" \
	"$script_dir/aarch64-godot-linux-gnu.ini.in" > "$cross_file"

export PYTHONPATH="$GSTREAMER_MESON_PYTHONPATH"
export PATH="$GSTREAMER_HOST_TOOLS:$VTB_GODOT_SDK/bin:$PATH"
export PKG_CONFIG_SYSROOT_DIR="$GSTREAMER_STAGING_ROOT"
export PKG_CONFIG_LIBDIR="$GSTREAMER_STAGING_ROOT$prefix/$libdir/pkgconfig:$GSTREAMER_STAGING_ROOT$prefix/share/pkgconfig:$target_sysroot/usr/lib/pkgconfig"

meson() {
	"$GSTREAMER_MESON_PYTHON" -m mesonbuild.mesonmain "$@"
}

setup_compile_install() {
	name=$1
	shift
	source_dir="$GSTREAMER_SOURCE_ROOT/$name-1.28.9"
	build_dir="$GSTREAMER_BUILD_ROOT/$name"
	setup_mode=
	if [ -f "$build_dir/meson-private/coredata.dat" ]; then
		setup_mode=--wipe
	fi
	meson setup $setup_mode "$build_dir" "$source_dir" \
		--cross-file "$cross_file" \
		--buildtype release \
		--prefix "$prefix" \
		--libdir "$libdir" \
		-Ddefault_library=shared \
		-Dauto_features=disabled \
		-Dtests=disabled \
		-Dexamples=disabled \
		-Ddoc=disabled \
		-Dnls=disabled \
		"$@"
	meson compile -C "$build_dir" -j "$GSTREAMER_BUILD_JOBS"
	DESTDIR="$GSTREAMER_STAGING_ROOT" meson install -C "$build_dir"
}

setup_compile_install gstreamer \
	-Dbenchmarks=disabled \
	-Dintrospection=disabled \
	-Dcheck=disabled \
	-Dlibunwind=disabled \
	-Dlibdw=disabled \
	-Dcoretracers=disabled \
	-Dptp-helper=disabled \
	-Dbash-completion=disabled \
	-Dtools=enabled

setup_compile_install gst-plugins-base \
	-Dintrospection=disabled \
	-Dorc=disabled \
	-Dapp=enabled \
	-Dtypefind=enabled

setup_compile_install gst-plugins-good \
	-Dorc=disabled \
	-Dv4l2=enabled \
	-Dv4l2-probe=false \
	-Dv4l2-libv4l2=disabled \
	-Dv4l2-gudev=disabled

setup_compile_install gst-plugins-bad \
	-Dintrospection=disabled \
	-Dorc=disabled \
	-Djpegformat=enabled \
	-Duvcgadget=enabled
