#!/usr/bin/env bash
set -euo pipefail

deploy_dir=${OPENVT_EMBEDDED_DEPLOY_DIR:-/home/huangnv/VTB/runtime/cpp-host-libgodot-20261030-01}
config_path=${OPENVT_CONFIG:-$deploy_dir/config/vtb-live2d.json}
opencv_dir=${OPENVT_OPENCV_LIB_DIR:-/home/huangnv/tools/godot-ayagami-live2d-cpp-face-0e951e6/opencv-lib}
mali_root=${OPENVT_MALI_ROOT:-/opt/rockchip/libmali-g13p0-x11-gbm-1.9-1/usr/lib/aarch64-linux-gnu}

for required_path in \
	"$deploy_dir/bin/vtb-live2d" \
	"$deploy_dir/bin/libgodot.linuxbsd.template_release.arm64.so" \
	"$config_path" \
	/dev/dri/renderD128 \
	/dev/dma_heap/system-uncached \
	/dev/rga \
	/dev/mpp_service; do
	if [[ ! -e "$required_path" ]]; then
		echo "required path is missing: $required_path" >&2
		exit 21
	fi
done

exec sudo -n env \
	-u DISPLAY \
	-u XAUTHORITY \
	-u MALI_DEFAULT_WINSYS \
	LD_LIBRARY_PATH="$deploy_dir/bin:$opencv_dir:$mali_root/mali:$mali_root:/usr/lib/aarch64-linux-gnu" \
	"$deploy_dir/bin/vtb-live2d" \
	--config "$config_path" \
	--resolution 1x1
