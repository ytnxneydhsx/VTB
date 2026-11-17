#!/usr/bin/env bash

set -eu

: "${GSTREAMER_SOURCE_ROOT:?Set the directory containing the five 1.28.9 source trees}"
: "${GSTREAMER_BUILD_ROOT:?Set a task-owned build directory}"
: "${GSTREAMER_STAGING_ROOT:?Set a task-owned DESTDIR/sysroot overlay}"
: "${GSTREAMER_MESON_PYTHON:?Set the absolute path to the host Python interpreter}"
: "${GSTREAMER_MESON_PYTHONPATH:?Set the unpacked Meson 1.7.2 wheel directory}"
: "${GSTREAMER_HOST_TOOLS:?Set the task-local directory containing GLib code generators}"
: "${GSTREAMER_GUDEV_ROOT:?Set the merged ARM64 libgudev runtime/development package root}"
: "${GSTREAMER_ROCKCHIP_SOURCE:?Set the pinned gstreamer-rockchip source directory}"
: "${GSTREAMER_OPENSSL_SOURCE:?Set the pinned OpenSSL 3.5.9 source directory}"
: "${GSTREAMER_LIBNICE_SOURCE:?Set the pinned libnice 0.1.24 source directory}"
: "${GSTREAMER_LIBSRTP_SOURCE:?Set the pinned libsrtp 2.8.0 source directory}"
: "${GSTREAMER_RS_SOURCE:?Set the pinned gst-plugins-rs 0.15 source directory}"
: "${GSTREAMER_RUST_ENV:?Set the Rust 1.94.0 env.sh path}"
: "${GSTREAMER_CARGO_HOME:?Set a task-owned Cargo home directory}"
: "${GSTREAMER_NODE_ROOT:?Set a task-owned Node.js root containing bin/node and bin/npm}"
: "${GSTREAMER_NPM_CACHE:?Set a task-owned npm cache directory}"
: "${VTB_GODOT_SDK:?Set the relocated Buildroot/Godot SDK root}"
: "${VTB_ROCKCHIP_SYSROOT:?Set the Rockchip board development sysroot}"
: "${GSTREAMER_BUILD_JOBS:?Set parallelism after checking server resources}"

prefix=/opt/vtb/gstreamer-1.28.9
libdir=lib/aarch64-linux-gnu
toolchain_root="$VTB_GODOT_SDK"
target_sysroot="$VTB_GODOT_SDK/aarch64-godot-linux-gnu/sysroot"
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cross_file="$GSTREAMER_BUILD_ROOT/aarch64-godot-linux-gnu.ini"
gst_plugins_bad_source="$GSTREAMER_SOURCE_ROOT/gst-plugins-bad-1.28.9"
gst_plugins_good_source="$GSTREAMER_SOURCE_ROOT/gst-plugins-good-1.28.9"
gst_rtsp_server_source="$GSTREAMER_SOURCE_ROOT/gst-rtsp-server-1.28.9"
gstreamer_rockchip_revision=bff0659a51c13585b3c9c9a68aa8c435beeb3fe1
openssl_revision=45e844fa2a14ec92d146bd8f5778ac130b6625fb
libnice_revision=af62014d520ac09c8fe1974bda1b253114c68843
libsrtp_revision=24b3bf8f19b6f5ab4cd2bcceb4f4064efca86fd5
gst_plugins_rs_revision=0f215b23ca52bc954db27262a061e7861a6f16c0

mkdir -p \
	"$GSTREAMER_BUILD_ROOT" \
	"$GSTREAMER_STAGING_ROOT" \
	"$GSTREAMER_CARGO_HOME" \
	"$GSTREAMER_NPM_CACHE"

check_revision() {
	source_dir=$1
	expected_revision=$2
	source_name=$3
	test -d "$source_dir/.git"
	actual_revision=$(git -C "$source_dir" rev-parse HEAD)
	if [ "$actual_revision" != "$expected_revision" ]; then
		printf '%s revision mismatch: expected %s, got %s\n' \
			"$source_name" "$expected_revision" "$actual_revision" >&2
		exit 1
	fi
}

apply_patch_set() {
	patch_source=$1
	shift
	for gstreamer_patch in "$@"; do
		if git -C "$patch_source" apply --check "$gstreamer_patch" >/dev/null 2>&1; then
			git -C "$patch_source" apply "$gstreamer_patch"
		elif git -C "$patch_source" apply --reverse --check "$gstreamer_patch" >/dev/null 2>&1; then
			printf 'GStreamer patch is already applied: %s\n' "$(basename "$gstreamer_patch")"
		else
			printf 'GStreamer patch is neither applicable nor already applied: %s\n' "$(basename "$gstreamer_patch")" >&2
			exit 1
		fi
	done
}

patch_set_is_applied() {
	patch_source=$1
	shift
	for gstreamer_patch in "$@"; do
		if ! git -C "$patch_source" apply --reverse --check \
			"$gstreamer_patch" >/dev/null 2>&1; then
			return 1
		fi
	done
}

patch_stamp="$GSTREAMER_SOURCE_ROOT/.vtb-gstreamer-patches-1.28.9.sha256"
patch_set_hash=$(
	sha256sum "$script_dir"/patches/000*.patch "$script_dir"/patches/100*.patch |
		cut -d ' ' -f 1 | sha256sum | cut -d ' ' -f 1
)

if [ -f "$patch_stamp" ]; then
	applied_patch_set_hash=$(cat "$patch_stamp")
	if [ "$applied_patch_set_hash" != "$patch_set_hash" ]; then
		if ! patch_set_is_applied \
			"$gst_plugins_bad_source" \
			"$script_dir"/patches/000*.patch; then
			printf 'GStreamer source was patched with a different project patch set.\n' >&2
			exit 1
		fi
		if ! patch_set_is_applied \
			"$gst_plugins_good_source" \
			"$script_dir"/patches/100*.patch; then
			printf 'GStreamer source was patched with a different project patch set.\n' >&2
			exit 1
		fi
		printf '%s\n' "$patch_set_hash" > "$patch_stamp"
		printf 'Migrated GStreamer patch stamp to the path-independent hash.\n'
	fi
	printf 'GStreamer project patch set is already applied.\n'
else
	apply_patch_set "$gst_plugins_bad_source" "$script_dir"/patches/000*.patch
	apply_patch_set "$gst_plugins_good_source" "$script_dir"/patches/100*.patch
	printf '%s\n' "$patch_set_hash" > "$patch_stamp"
fi

check_revision \
	"$GSTREAMER_ROCKCHIP_SOURCE" \
	"$gstreamer_rockchip_revision" \
	gstreamer-rockchip
check_revision "$GSTREAMER_OPENSSL_SOURCE" "$openssl_revision" OpenSSL
check_revision "$GSTREAMER_LIBNICE_SOURCE" "$libnice_revision" libnice
check_revision "$GSTREAMER_LIBSRTP_SOURCE" "$libsrtp_revision" libsrtp
check_revision \
	"$GSTREAMER_RS_SOURCE" \
	"$gst_plugins_rs_revision" \
	gst-plugins-rs

rockchip_patch_stamp="$GSTREAMER_ROCKCHIP_SOURCE/.vtb-gstreamer-rockchip-patches.sha256"
rockchip_patch_set_hash=$(
	sha256sum "$script_dir"/patches/200*.patch |
		cut -d ' ' -f 1 | sha256sum | cut -d ' ' -f 1
)
if [ -f "$rockchip_patch_stamp" ]; then
	applied_rockchip_patch_set_hash=$(cat "$rockchip_patch_stamp")
	if [ "$applied_rockchip_patch_set_hash" != "$rockchip_patch_set_hash" ]; then
		if ! patch_set_is_applied \
			"$GSTREAMER_ROCKCHIP_SOURCE" \
			"$script_dir"/patches/200*.patch; then
			printf 'gstreamer-rockchip was patched with a different project patch set.\n' >&2
			exit 1
		fi
		printf '%s\n' "$rockchip_patch_set_hash" > "$rockchip_patch_stamp"
		printf 'Migrated gstreamer-rockchip patch stamp to the path-independent hash.\n'
	fi
	printf 'gstreamer-rockchip project patch set is already applied.\n'
else
	apply_patch_set "$GSTREAMER_ROCKCHIP_SOURCE" "$script_dir"/patches/200*.patch
	printf '%s\n' "$rockchip_patch_set_hash" > "$rockchip_patch_stamp"
fi

test -f "$VTB_ROCKCHIP_SYSROOT/usr/include/rockchip/rk_mpi.h"
test -f "$VTB_ROCKCHIP_SYSROOT/usr/lib/aarch64-linux-gnu/librockchip_mpp.so"
test -f "$VTB_ROCKCHIP_SYSROOT/usr/include/rga/RgaApi.h"
test -f "$VTB_ROCKCHIP_SYSROOT/usr/lib/aarch64-linux-gnu/librga.so"

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
export PATH="$GSTREAMER_NODE_ROOT/bin:$GSTREAMER_HOST_TOOLS:$VTB_GODOT_SDK/bin:$PATH"
export PKG_CONFIG_SYSROOT_DIR="$GSTREAMER_STAGING_ROOT"
export PKG_CONFIG_LIBDIR="$GSTREAMER_STAGING_ROOT$prefix/$libdir/pkgconfig:$GSTREAMER_STAGING_ROOT$prefix/share/pkgconfig:$target_sysroot/usr/lib/pkgconfig"

test -x "$GSTREAMER_NODE_ROOT/bin/node"
test -x "$GSTREAMER_NODE_ROOT/bin/npm"
node_major=$(node --version | sed -e 's/^v//' -e 's/\..*$//')
if [ "$node_major" -lt 18 ]; then
	printf 'Node.js 18 or newer is required to build gstwebrtc-api.\n' >&2
	exit 1
fi

rga_development_root="$GSTREAMER_STAGING_ROOT$prefix"
mkdir -p \
	"$rga_development_root/include" \
	"$rga_development_root/$libdir/pkgconfig"
cp -a "$VTB_ROCKCHIP_SYSROOT/usr/include/rga" \
	"$rga_development_root/include/"
cp -a \
	"$VTB_ROCKCHIP_SYSROOT/usr/lib/aarch64-linux-gnu/librga.so" \
	"$rga_development_root/$libdir/librga.so"
sed \
	-e "s|@PREFIX@|$prefix|g" \
	"$script_dir/librga.pc.in" \
	> "$rga_development_root/$libdir/pkgconfig/librga.pc"

meson() {
	"$GSTREAMER_MESON_PYTHON" -m mesonbuild.mesonmain "$@"
}

openssl_build_dir="$GSTREAMER_BUILD_ROOT/openssl"
mkdir -p "$openssl_build_dir"
if [ ! -f "$openssl_build_dir/Makefile" ]; then
	(
		cd "$openssl_build_dir"
		"$GSTREAMER_OPENSSL_SOURCE/Configure" \
			linux-aarch64 \
			--prefix="$prefix" \
			--libdir="$libdir" \
			--cross-compile-prefix="$VTB_GODOT_SDK/bin/aarch64-godot-linux-gnu-" \
			shared \
			no-apps \
			no-tests
	)
fi
make -C "$openssl_build_dir" -j "$GSTREAMER_BUILD_JOBS"
make -C "$openssl_build_dir" \
	DESTDIR="$GSTREAMER_STAGING_ROOT" \
	install_sw

setup_dependency_compile_install() {
	name=$1
	source_dir=$2
	shift 2
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
		"$@"
	meson compile -C "$build_dir" -j "$GSTREAMER_BUILD_JOBS"
	DESTDIR="$GSTREAMER_STAGING_ROOT" meson install -C "$build_dir"
}

setup_dependency_compile_install \
	libsrtp \
	"$GSTREAMER_LIBSRTP_SOURCE" \
	-Dcrypto-library=openssl \
	-Dcrypto-library-kdf=disabled \
	-Dtests=disabled \
	-Dpcap-tests=disabled \
	-Ddoc=disabled

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
		"$@"
	meson compile -C "$build_dir" -j "$GSTREAMER_BUILD_JOBS"
	DESTDIR="$GSTREAMER_STAGING_ROOT" meson install -C "$build_dir"
}

setup_compile_install gstreamer \
	-Dnls=disabled \
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
	-Dnls=disabled \
	-Dintrospection=disabled \
	-Dorc=disabled \
	-Dapp=enabled \
	-Dtypefind=enabled \
	-Dvideorate=enabled

# Build libnice after GStreamer base and net so it can install nicesrc/nicesink.
setup_dependency_compile_install \
	libnice \
	"$GSTREAMER_LIBNICE_SOURCE" \
	-Dgupnp=disabled \
	-Dgstreamer=enabled \
	-Dcrypto-library=openssl \
	-Dexamples=disabled \
	-Dtests=disabled \
	-Dgtk_doc=disabled \
	-Dintrospection=disabled

setup_compile_install gst-plugins-good \
	-Dnls=disabled \
	-Dorc=disabled \
	-Drtp=enabled \
	-Drtpmanager=enabled \
	-Drtsp=enabled \
	-Dudp=enabled \
	-Dv4l2=enabled \
	-Dv4l2-probe=false \
	-Dv4l2-libv4l2=disabled \
	-Dv4l2-gudev=disabled

setup_compile_install gst-plugins-bad \
	-Dnls=disabled \
	-Dintrospection=disabled \
	-Dorc=disabled \
	-Ddebugutils=enabled \
	-Djpegformat=enabled \
	-Dvideoparsers=enabled \
	-Duvcgadget=enabled \
	-Ddtls=enabled \
	-Dsctp=enabled \
	-Dsctp-internal-usrsctp=enabled \
	-Dsrtp=enabled \
	-Dwebrtc=enabled

test -d "$gst_rtsp_server_source"
setup_compile_install gst-rtsp-server \
	-Dintrospection=disabled \
	-Drtspclientsink=disabled

rockchip_build_dir="$GSTREAMER_BUILD_ROOT/gstreamer-rockchip"
rockchip_setup_mode=
if [ -f "$rockchip_build_dir/meson-private/coredata.dat" ]; then
	rockchip_setup_mode=--wipe
fi
meson setup $rockchip_setup_mode \
	"$rockchip_build_dir" "$GSTREAMER_ROCKCHIP_SOURCE" \
	--cross-file "$cross_file" \
	--buildtype release \
	--prefix "$prefix" \
	--libdir "$libdir" \
	-Ddefault_library=shared \
	-Drkximage=disabled \
	-Drockchipmpp=enabled \
	-Drockchipmpp-root="$VTB_ROCKCHIP_SYSROOT" \
	-Dkmssrc=disabled \
	-Drga=enabled \
	-Dgpu=disabled \
	-Dvpxalphadec=disabled \
	-Dgobject-cast-checks=disabled \
	-Dglib-asserts=disabled \
	-Dpackage-name='VTB Rockchip GStreamer plug-in' \
	-Dpackage-origin='https://github.com/JeffyCN/mirrors/tree/gstreamer-rockchip'
meson compile -C "$rockchip_build_dir" -j "$GSTREAMER_BUILD_JOBS"
DESTDIR="$GSTREAMER_STAGING_ROOT" meson install -C "$rockchip_build_dir"

. "$GSTREAMER_RUST_ENV"
export CARGO_HOME="$GSTREAMER_CARGO_HOME"
export CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER="$VTB_GODOT_SDK/bin/aarch64-godot-linux-gnu-gcc"
export CC_aarch64_unknown_linux_gnu="$VTB_GODOT_SDK/bin/aarch64-godot-linux-gnu-gcc"
export CXX_aarch64_unknown_linux_gnu="$VTB_GODOT_SDK/bin/aarch64-godot-linux-gnu-g++"
export AR_aarch64_unknown_linux_gnu="$VTB_GODOT_SDK/bin/aarch64-godot-linux-gnu-ar"
export PKG_CONFIG_ALLOW_CROSS=1
export RUSTFLAGS="-C link-arg=-Wl,-rpath-link,$GSTREAMER_STAGING_ROOT$prefix/$libdir"
export CARGO_TARGET_DIR="$GSTREAMER_BUILD_ROOT/gst-plugins-rs-target"
export CARGO_NET_GIT_FETCH_WITH_CLI=true
export CARGO_NET_RETRY=10
export CARGO_HTTP_TIMEOUT=120

cargo build \
	--manifest-path "$GSTREAMER_RS_SOURCE/Cargo.toml" \
	--target aarch64-unknown-linux-gnu \
	--release \
	--locked \
	--package gst-plugin-webrtc \
	--no-default-features \
	--features web_server,rustls-ring

rswebrtc_plugin="$CARGO_TARGET_DIR/aarch64-unknown-linux-gnu/release/libgstrswebrtc.so"
test -f "$rswebrtc_plugin"
mkdir -p "$GSTREAMER_STAGING_ROOT$prefix/$libdir/gstreamer-1.0"
cp -a \
	"$rswebrtc_plugin" \
	"$GSTREAMER_STAGING_ROOT$prefix/$libdir/gstreamer-1.0/"

webrtc_web_root="$GSTREAMER_RS_SOURCE/net/webrtc/gstwebrtc-api"
(
	cd "$webrtc_web_root"
	npm install \
		--cache "$GSTREAMER_NPM_CACHE" \
		--ignore-scripts \
		--no-audit \
		--no-fund
	npm run build
)
test -f "$webrtc_web_root/dist/index.html"
mkdir -p "$GSTREAMER_STAGING_ROOT$prefix/share/gstwebrtc-api"
cp -a \
	"$webrtc_web_root/dist" \
	"$GSTREAMER_STAGING_ROOT$prefix/share/gstwebrtc-api/"
