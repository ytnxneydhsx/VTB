#!/bin/bash
set -eu

probe_bin=${PROBE_BIN:-/home/huangnv/openvt-dmabuf-probe/dmabuf-rga-mpp-probe}
output_dir=${PROBE_OUTPUT_DIR:-/home/huangnv/openvt-dmabuf-probe-output-20261027-01}
output_file="$output_dir/dmabuf-rga-mpp-1920x1080-30fps.h264"
mp4_file="$output_dir/dmabuf-rga-mpp-1920x1080-30fps.mp4"
first_frame="$output_dir/first-frame.png"
dma_heap=${DMA_HEAP_PATH:-/dev/dma_heap/system-uncached}
mali_root=/opt/rockchip/libmali-g13p0-x11-gbm-1.9-1/usr/lib/aarch64-linux-gnu

if [ -e "$output_dir" ]; then
	echo "output directory already exists: $output_dir" >&2
	exit 20
fi

for required_path in \
	"$probe_bin" \
	"$mali_root/mali/libEGL.so.1" \
	"$dma_heap" \
	/dev/rga \
	/dev/mpp_service; do
	if [ ! -e "$required_path" ]; then
		echo "required path is missing: $required_path" >&2
		exit 21
	fi
done

mkdir -p "$output_dir"

{
	echo "date=$(date --iso-8601=seconds)"
	echo "hostname=$(hostname)"
	echo "architecture=$(uname -m)"
	echo "kernel=$(uname -r)"
	echo "probe_bin=$probe_bin"
	echo "output_file=$output_file"
	echo "mp4_file=$mp4_file"
	echo "display=:0"
	echo "mali_default_winsys=x11"
	echo "dma_heap=$dma_heap"
	sha256sum "$probe_bin"
} > "$output_dir/run-manifest.txt"

set +e
sudo -n env \
	DISPLAY=:0 \
	XAUTHORITY=/home/huangnv/.Xauthority \
	MALI_DEFAULT_WINSYS=x11 \
	LD_LIBRARY_PATH="$mali_root/mali:$mali_root:/usr/lib/aarch64-linux-gnu" \
	"$probe_bin" "$output_file" "$dma_heap" \
	> "$output_dir/probe.log" 2>&1
probe_exit=$?
set -e

printf '%d\n' "$probe_exit" > "$output_dir/probe.exit"
if [ "$probe_exit" -ne 0 ]; then
	echo "probe failed with exit code $probe_exit" >&2
	exit "$probe_exit"
fi

ffmpeg -hide_banner -loglevel error -y \
	-framerate 30 -i "$output_file" -c copy "$mp4_file"

ffprobe \
	-v error \
	-count_frames \
	-select_streams v:0 \
	-show_entries stream=codec_name,width,height,pix_fmt,r_frame_rate,avg_frame_rate,nb_read_frames:format=duration,size \
	-of default=noprint_wrappers=1 \
	"$mp4_file" \
	> "$output_dir/ffprobe.txt"

ffmpeg -hide_banner -loglevel error -y -i "$mp4_file" -f null - \
	> "$output_dir/decode.log" 2>&1

ffmpeg -hide_banner -loglevel error -y -i "$mp4_file" -frames:v 1 "$first_frame"

sha256sum \
	"$output_file" \
	"$mp4_file" \
	"$first_frame" \
	"$output_dir/probe.log" \
	"$output_dir/ffprobe.txt" \
	"$output_dir/decode.log" \
	> "$output_dir/SHA256SUMS"

echo "probe complete: $output_file"
