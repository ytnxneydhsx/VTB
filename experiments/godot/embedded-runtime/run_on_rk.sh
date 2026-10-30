#!/usr/bin/env bash
set -euo pipefail

deploy_dir=${OPENVT_EMBEDDED_DEPLOY_DIR:-/home/huangnv/VTB/runtime/cpp-host-libgodot-20261030-01}
project_dir=${OPENVT_GODOT_PROJECT_DIR:-$deploy_dir/project}
model_path=${OPENVT_LIVE2D_MODEL:-/home/huangnv/open-vt-model/阿芙洛狄忒模型文件/fense_vts/fense.model3.json}
opencv_dir=${OPENVT_OPENCV_LIB_DIR:-/home/huangnv/tools/godot-ayagami-live2d-cpp-face-0e951e6/opencv-lib}
mali_root=${OPENVT_MALI_ROOT:-/opt/rockchip/libmali-g13p0-x11-gbm-1.9-1/usr/lib/aarch64-linux-gnu}

for required_path in \
	"$deploy_dir/bin/vtb-live2d" \
	"$deploy_dir/bin/libgodot.linuxbsd.template_release.arm64.so" \
	"$project_dir/project.godot" \
	"$project_dir/.godot/extension_list.cfg" \
	"$project_dir/.godot/global_script_class_cache.cfg" \
	"$project_dir/addons/ayagami/ayagami.gdextension" \
	"$model_path" \
	"${OPENVT_FACE_SOURCE:-/dev/video0}" \
	"${OPENVT_FACE_DETECTOR_MODEL:-/home/huangnv/VTB/models/openseeface/mnv3_detection_fp16.rknn}" \
	"${OPENVT_FACE_LANDMARK_MODEL:-/home/huangnv/VTB/models/openseeface/lm_model3_fp16.rknn}" \
	/home/huangnv/.Xauthority \
	/dev/dma_heap/system-uncached \
	/dev/rga \
	/dev/mpp_service; do
	if [[ ! -e "$required_path" ]]; then
		echo "required path is missing: $required_path" >&2
		exit 21
	fi
done

exec sudo -n env \
	OPENVT_GODOT_PROJECT_DIR="$project_dir" \
	OPENVT_LIVE2D_MODEL="$model_path" \
	OPENVT_RTSP_URL="${OPENVT_RTSP_URL:-rtsp://127.0.0.1:8554/live2d}" \
	OPENVT_FACE_SOURCE="${OPENVT_FACE_SOURCE:-/dev/video0}" \
	OPENVT_FACE_WIDTH="${OPENVT_FACE_WIDTH:-1280}" \
	OPENVT_FACE_HEIGHT="${OPENVT_FACE_HEIGHT:-720}" \
	OPENVT_FACE_FPS="${OPENVT_FACE_FPS:-60}" \
	OPENVT_FACE_BUFFERS="${OPENVT_FACE_BUFFERS:-4}" \
	OPENVT_FACE_DETECTOR_MODEL="${OPENVT_FACE_DETECTOR_MODEL:-/home/huangnv/VTB/models/openseeface/mnv3_detection_fp16.rknn}" \
	OPENVT_FACE_LANDMARK_MODEL="${OPENVT_FACE_LANDMARK_MODEL:-/home/huangnv/VTB/models/openseeface/lm_model3_fp16.rknn}" \
	OPENVT_FACE_CALIBRATION_FRAMES="${OPENVT_FACE_CALIBRATION_FRAMES:-30}" \
	OPENVT_FACE_SMOOTHING_MS="${OPENVT_FACE_SMOOTHING_MS:-50}" \
	OPENVT_FACE_ANGLE_X_SIGN="${OPENVT_FACE_ANGLE_X_SIGN:--1}" \
	OPENVT_FACE_ANGLE_Y_SIGN="${OPENVT_FACE_ANGLE_Y_SIGN:--1}" \
	OPENVT_FACE_ANGLE_Z_SIGN="${OPENVT_FACE_ANGLE_Z_SIGN:--1}" \
	OPENVT_LIVE2D_CONTROL_ADDRESS="${OPENVT_LIVE2D_CONTROL_ADDRESS:-192.168.77.2}" \
	OPENVT_LIVE2D_CONTROL_PORT="${OPENVT_LIVE2D_CONTROL_PORT:-11575}" \
	OPENVT_LIVE2D_CONTROL_ORIGIN="${OPENVT_LIVE2D_CONTROL_ORIGIN:-http://192.168.77.1:8900}" \
	OPENVT_LIVE2D_TOTAL_FRAMES="${OPENVT_LIVE2D_TOTAL_FRAMES:-0}" \
	OPENVT_OUTPUT_WIDTH="${OPENVT_OUTPUT_WIDTH:-1920}" \
	OPENVT_OUTPUT_HEIGHT="${OPENVT_OUTPUT_HEIGHT:-1080}" \
	OPENVT_OUTPUT_FPS="${OPENVT_OUTPUT_FPS:-30}" \
	DISPLAY=:0 \
	XAUTHORITY=/home/huangnv/.Xauthority \
	MALI_DEFAULT_WINSYS=x11 \
	LD_LIBRARY_PATH="$deploy_dir/bin:$opencv_dir:$mali_root/mali:$mali_root:/usr/lib/aarch64-linux-gnu" \
	"$deploy_dir/bin/vtb-live2d" \
	--resolution 1x1 \
	--position 1200,0
